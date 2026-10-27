#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <schemata/core/types.h>
#include <schemata/driver/driver.h>
#include <schemata/sql/dialect.h>

namespace schemata {

/// One column as it exists in a live database.
struct SnapshotColumn {
    std::string name;

    /// The engine's type, normalised into Schemata's vocabulary.
    ///
    /// Necessarily lossy on SQLite, which has affinities rather than types
    /// and cannot distinguish a boolean from an integer or a timestamp from
    /// any other text. The differ compares at whatever granularity the engine
    /// can actually represent -- see `comparable_type`.
    ColumnType type = ColumnType::text;

    /// The type exactly as the engine spelled it, kept for scaffolding.
    ///
    /// `VARCHAR(255)` and `TEXT` normalise to the same ColumnType, but only
    /// one of them is what the database actually says.
    std::string declared_type;

    bool nullable    = false;
    bool primary_key = false;

    /// Whether the engine generates this column's value when an insert leaves
    /// it out: `AUTO_INCREMENT` on the MySQL family, and on SQLite the rowid
    /// alias -- a rowid table's single-column primary key declared exactly
    /// `INTEGER` -- whether or not AUTOINCREMENT follows it.
    bool identity = false;

    /// SQLite only: identity declared with AUTOINCREMENT.
    ///
    /// The keyword changes one thing about a rowid alias: a key is never handed
    /// out twice, even after the row holding the highest one is deleted. It is
    /// kept apart from `identity` so a plain alias is not mistaken for a column
    /// the engine does not generate, which is what the application's inserts
    /// then find out the hard way.
    bool autoincrement = false;

    std::optional<int>         max_length;
    std::optional<std::string> default_sql;

    // The same shape a model column carries, read back off the server. Every
    // one of these has to survive the round trip or the differ sees drift on
    // a database that already matches -- see `column_matches`.
    bool                     fixed_length = false;
    std::optional<int>       precision;
    std::optional<int>       scale;
    std::vector<std::string> allowed;
    bool                     on_update_now = false;

    /// Whether the server computes this column from an expression.
    ///
    /// Read on the MySQL family, from `EXTRA`. Such a column cannot be written
    /// -- an INSERT naming it is refused by MySQL and by MariaDB -- and a
    /// model has no read-only column to map it to, so the scaffolder leaves
    /// it out, and the differ does not propose dropping it for being absent
    /// from the model. SQLite's `pragma_table_info` does not report generated
    /// columns at all, so none is marked there.
    bool generated = false;

    /// The collation the engine reports, as the engine spells it.
    ///
    /// Unlike everything else here this is not normalised into a Schemata
    /// vocabulary, because there is no portable one to normalise into: the
    /// three engines share no collation names and the correspondence between
    /// them depends on the data rather than on the engines.
    ///
    /// Read for two reasons. A model that declares a collation is compared
    /// against it; a model that does not has this carried back into any
    /// statement that rewrites the column, so the rewrite does not silently
    /// reset it.
    std::string collation;
};

struct SnapshotIndex {
    std::string              name;
    std::vector<std::string> columns;
    bool                     unique = false;

    /// Whether a model could declare this index at all.
    ///
    /// Three shapes no model can express, and each of them read as an index
    /// the model had dropped -- a destructive drop, on every run, which had
    /// every later change to the table refused with it. They are left out of
    /// the diff instead: `expression` is an index over an expression rather
    /// than plain columns, `partial` one that indexes only some rows
    /// (SQLite's `CREATE INDEX ... WHERE`), and `prefix` one over the first n
    /// characters of a column (MySQL's `name(10)`).
    bool expression = false;
    bool partial    = false;
    bool prefix     = false;

    /// Per key column: whether the index sorts it descending, and the
    /// collation it names when that is not the column's own.
    ///
    /// Parallel to `columns`, or empty where the engine was not asked. SQLite
    /// only. The model says neither, so both used to disappear the first time
    /// anything rebuilt the table -- `CREATE INDEX ix ON t(name COLLATE NOCASE
    /// DESC)` came back as `CREATE INDEX ix ON t("name")`, which is a
    /// different index for every query that used it.
    std::vector<bool>        descending;
    std::vector<std::string> collations;

    /// True when this index is the primary key rather than a separate object.
    ///
    /// Both engines report the primary key as an index. Treating it as one
    /// would make the differ try to drop and recreate it on every run.
    bool primary = false;

    /// True when this index exists only to back a constraint.
    ///
    /// InnoDB creates an index for a foreign key whose columns are not
    /// already indexed, and names it after the constraint; SQLite creates
    /// one for an inline UNIQUE. Neither is a separate object: the model
    /// never declares it, the server refuses to drop it while the
    /// constraint stands, and diffing it would propose a destructive drop
    /// on every run -- which makes ensure_schema refuse the whole diff,
    /// legitimate operations included.
    ///
    /// On the MySQL family the flag is read off the name alone: an index
    /// named as a foreign key on its table is. Nothing the server reports
    /// tells the index InnoDB made from an ordinary `KEY` of that name, which
    /// is what a dump writes InnoDB's out as and a restore creates.
    ///
    /// And the server refuses the drop (1553) only while nothing else leads
    /// with the key's columns. Once another index does, InnoDB takes its own
    /// away by itself and lets an explicit one be dropped -- so where the
    /// model's primary key or one of its declared indexes covers the key,
    /// the differ stops hiding an index of InnoDB's shape (not unique, over
    /// exactly the key's columns) and drops it, as a widening: a fresh build
    /// has no such index, and nothing rests on this one.
    bool constraint_backed = false;
};

struct SnapshotForeignKey {
    std::string              name;
    std::vector<std::string> columns;

    std::string              referenced_table;
    std::vector<std::string> referenced_columns;
    ReferentialAction        on_delete = ReferentialAction::no_action;
    ReferentialAction        on_update = ReferentialAction::no_action;

    /// SQLite only: declared `DEFERRABLE INITIALLY DEFERRED`.
    ///
    /// The one spelling that actually defers the check to COMMIT, which is
    /// what lets an application write a cycle of rows in any order. No model
    /// declares it, so it is read to be named rather than to be compared: a
    /// rebuild regenerates the constraint without it, and the writes that
    /// relied on it start failing.
    bool deferred = false;

    /// The schema holding the referenced table, as the server names it, or
    /// empty when it is the schema being read. MySQL family only.
    ///
    /// The server's name and not a model's alias: the reader has no model.
    /// The differ binds the model's aliases to names before it compares, so
    /// the two meet on the name. A key into a schema no binding names is read
    /// all the same, so that a key nobody modelled is still seen. Last, and
    /// initialised, so an initialiser written before it existed still means
    /// what it meant and still compiles clean.
    std::string referenced_schema{};
};

/// A foreign key another schema holds onto a table of this one. MySQL family
/// only.
///
/// Not this schema's to change, and read for a single reason: the server
/// refuses to drop the table it references (3730), or to drop or retype the
/// columns and the index it stands on, while the key is there. Knowing that
/// beforehand lets `ensure_schema()` name the key and where it lives instead of
/// stopping partway through a plan on the server's error.
struct SnapshotInboundKey {
    std::string              schema;  // where the key is, as the server names it
    std::string              table;
    std::string              name;
    std::vector<std::string> columns;             // on that table
    std::vector<std::string> referenced_columns;  // on this one
};

/// A trigger as the database holds it. SQLite only.
struct SnapshotTrigger {
    std::string name;

    /// The `CREATE TRIGGER` text, exactly as `sqlite_master` stores it.
    std::string sql;

    /// Whether this is the trigger standing in for `on_update_now`.
    ///
    /// One Schemata wrote and Schemata owns: a rebuild regenerates it from
    /// the model rather than replaying the stored text, so the column's
    /// current declaration is what ends up in the database. The rest are the
    /// user's and go back exactly as they were found.
    bool modelled = false;

    /// Whether this is the library's trigger in the text an older release
    /// wrote.
    ///
    /// Still the library's -- a rebuild regenerates it and never replays it --
    /// but not the text the model means any more, so the column it stamps does
    /// not read as carrying the clause. The differ replaces it with the
    /// current text, which is a heal rather than a loss: the same clause,
    /// keyed on the primary key instead of on rowid.
    bool outdated = false;
};

/// A CHECK constraint as the database holds it.
struct SnapshotCheck {
    /// The constraint's name, or empty where it has none.
    ///
    /// SQLite gives one a name only where the statement said `CONSTRAINT x
    /// CHECK (...)`, which a hand-written migration rarely does. The servers
    /// always have one, inventing `<table>_chk_1` where nobody chose it.
    ///
    /// Needed to drop the constraint on the MySQL family, and for nothing
    /// else -- two CHECKs are the same rule when they say the same thing, and
    /// a name is exactly what a constraint an existing database already has
    /// will not have in common with the model adopting it.
    std::string name;

    /// The column it was declared on, or empty for a table constraint.
    ///
    /// SQLite only, read out of the stored `CREATE TABLE`. Neither server
    /// records which column a check was written beside, so this is empty
    /// there even for one that names a single column -- which is why the
    /// differ matches on the table's whole set rather than column by column.
    std::string column;

    /// The expression, without the keyword or its parentheses, as the engine
    /// holds it -- which is not how it was written. See
    /// `schemata::Column::check`.
    std::string expression;

    /// MariaDB only: written beside a column rather than as a table
    /// constraint (`LEVEL = 'Column'` in its catalog). MariaDB names such a
    /// CHECK after the column and will not `DROP CONSTRAINT` it; redefining
    /// the column is what removes it.
    bool column_level = false;
};

/// The server's print of a CHECK the model declares, where the differ asked
/// for one. See `SnapshotTable::canonical_checks`.
struct SnapshotCheckPrint {
    /// The model's expression, as the model writes it.
    std::string expression;

    /// The rule as `SHOW CREATE TABLE` printed it. Empty where the server
    /// could not be asked.
    std::string printed;

    /// Why the server could not be asked, in its own words -- a session that
    /// may not make a temporary table, a transaction started READ ONLY (1792)
    /// on either server, a rule it refuses there. Empty where it printed the
    /// rule.
    std::string unprinted{};
};

struct SnapshotTable {
    std::string                     name;
    std::vector<SnapshotColumn>     columns;
    std::vector<SnapshotIndex>      indexes;
    std::vector<SnapshotForeignKey> foreign_keys;

    /// The AUTOINCREMENT high-water mark, when the table has one.
    ///
    /// SQLite keeps it in `sqlite_sequence`, and `DROP TABLE` deletes the row
    /// along with the table. A rebuild that did not put it back would let the
    /// next insert reuse an id a deleted row once had -- which is the one
    /// thing AUTOINCREMENT exists to prevent, and the reason Schemata emits
    /// it rather than settling for a plain rowid alias.
    std::optional<std::int64_t> identity_sequence;

    /// The table's default collation, as the engine spells it.
    ///
    /// Diffed only where the model gives a default of its own
    /// (`Model::default_collation`); otherwise read for the scaffolder. What
    /// it buys the scaffolder is judgement about
    /// which columns are worth scaffolding a collation for -- a column that
    /// merely inherits its table's default is not making a claim, and
    /// reproducing it explicitly would turn three deliberate columns into
    /// three hundred lines of noise.
    ///
    /// Empty on SQLite, which has no table default; a column there either
    /// names a collation or is BINARY.
    std::string collation;

    /// Whether this is a virtual table: FTS5, R*Tree and the like. SQLite only.
    ///
    /// Its columns are whatever its module declares, it has no primary key,
    /// and no model can create or alter one, so the scaffolder writes no
    /// entity for it.
    bool virtual_table = false;

    /// The virtual table this one keeps its data for, or empty. SQLite only.
    ///
    /// A virtual table's module stores what it holds in ordinary tables named
    /// after it -- `notes_fts_data`, `notes_fts_idx` and three more for FTS5 --
    /// which only the module may write. Recognised by the suffixes SQLite's
    /// own modules use, and only beside a virtual table of the name they
    /// extend, so an application's `notes_fts_archive` is not taken for one.
    std::string shadow_of;

    /// True for a MariaDB table `WITH SYSTEM VERSIONING`.
    ///
    /// Such a table keeps its own history, and the server refuses to add,
    /// change or drop a column on it, to move its primary key, or to create a
    /// unique index over it, unless `@@system_versioning_alter_history` says
    /// what should become of the history (4119). A plain index, a foreign key,
    /// a rename and a drop it allows. Read so that `ensure_schema()` and
    /// `verify_schema()` can say that before the server does. False everywhere
    /// else, MySQL included, which has no such table.
    bool system_versioned = false;

    /// The storage engine, as the server names it. MySQL family only.
    ///
    /// Read because engines are not alike to a foreign key: InnoDB enforces
    /// one, while MyISAM, Aria and MEMORY accept the definition and keep only
    /// an index. `ensure_schema()` and `verify_schema()` refuse a model key on
    /// or to a table on such an engine rather than report it applied. Never
    /// diffed otherwise. Empty on SQLite and in a snapshot built by hand, and
    /// empty counts as enforcing.
    std::string engine;

    /// Every trigger attached to this table.
    ///
    /// SQLite only, and read for two reasons. A table rebuild drops the
    /// table, which drops its triggers with it, so the procedure has to put
    /// them back: Schemata does not model triggers and never diffs them, and
    /// this is what it takes not to destroy one silently. And a trigger name
    /// is scoped to the database rather than to the table, so the name
    /// standing in for `on_update_now` on one table can be held by a trigger
    /// on another -- which has to be visible before it can be dealt with.
    std::vector<SnapshotTrigger> triggers;

    // Everything below is SQLite, read out of the `CREATE TABLE` text the
    // engine stores verbatim. None of it is ever compared against a model,
    // which has no words for any of it. It is read so that a rebuild -- which
    // regenerates the table from the model -- can say what it would take away
    // instead of taking it away quietly. See `rebuild_losses` in the differ.

    /// Every CHECK the table declares that the library did not derive.
    ///
    /// The two it does derive -- `one_of` and the constraint that makes a
    /// column unsigned -- are recognised on the column they constrain and read
    /// back into `allowed` and the column's type, so they never appear here.
    /// What is left is either a CHECK the model declares, which the differ
    /// matches against `Column::check` and `Table::checks`, or one belonging
    /// to somebody else, which a rebuild would take away.
    std::vector<SnapshotCheck> checks;

    /// The names of the table's generated columns.
    ///
    /// `pragma_table_info` leaves them out, so they are invisible to the rest
    /// of the snapshot. No model declares one, and a rebuild loses both the
    /// column and any index over it.
    std::vector<std::string> generated_columns;

    /// Every ON CONFLICT clause but the default, with what carries it.
    std::vector<std::string> conflict_clauses;

    /// Table options, which live after the closing parenthesis of the
    /// `CREATE TABLE` -- where a regenerated statement has nothing. Losing the
    /// first changes how every row is stored and what the old application's
    /// queries cost; losing the second stops the table refusing text in an
    /// INTEGER column.
    bool without_rowid = false;
    bool strict        = false;

    /// Find a column or an index by name, without regard to ASCII case -- the
    /// way every engine here compares those names.
    /// Keys in other schemas that reference this table. MySQL family only.
    ///
    /// Last among the members, so an initialiser written before it existed
    /// still means what it meant.
    std::vector<SnapshotInboundKey> referenced_from{};

    /// MySQL family only: how the server prints a CHECK the model declares on
    /// this table, where the differ had to ask. Filled by `ensure_schema()` and
    /// `verify_schema()`, never by `read_schema()`, and only for a model CHECK
    /// under the name of a live one whose expression reads as another rule
    /// here -- a spelling the normaliser does not know the server's print of,
    /// such as MariaDB's `LENGTH` printed `octet_length`, or MySQL's `CAST`
    /// printed with a character set. The server is asked by building the rule
    /// on a temporary table and reading it back. The differ compares the print
    /// rather than the model's text, so a rule spelled another way is not
    /// taken for a redefined one. Where the server could not be asked, the
    /// CHECK is compared as the model writes it, and what the differ proposes
    /// for it says why. Empty, every CHECK is compared as the model writes it.
    std::vector<SnapshotCheckPrint> canonical_checks{};

    [[nodiscard]] const SnapshotColumn*     find_column(std::string_view wanted) const;
    [[nodiscard]] const SnapshotIndex*      find_index(std::string_view wanted) const;
    [[nodiscard]] const SnapshotForeignKey* find_foreign_key(std::string_view wanted) const;
    [[nodiscard]] std::vector<const SnapshotColumn*> key_columns() const;
};

/// A view as the database holds it. SQLite only.
///
/// A view is not a table and Schemata does not model one, so views take no
/// part in a diff. They are read for a single purpose: a column a view names
/// cannot be taken away without breaking the view, and SQLite's own check for
/// that is switched off during a table rebuild.
struct SnapshotView {
    std::string name;
    std::string sql;

    /// Every identifier and bare word the statement uses, unquoted.
    ///
    /// Read as tokens, so a name inside a string literal or a comment is not
    /// one of these and a quoted name is the name itself.
    std::vector<std::string> identifiers;

    /// Whether the statement names this identifier, as SQLite compares names.
    ///
    /// Deliberately blunt: it answers "somewhere in this view" rather than
    /// "as a column of that table", because the second means resolving names
    /// through every join and subquery the view may contain. The cost of
    /// being told to look at a view that did not need changing is a minute;
    /// the cost of the other mistake is a read path that fails in production.
    [[nodiscard]] bool mentions(std::string_view identifier) const;
};

/// A live schema, read back.
struct SchemaSnapshot {
    Dialect                    dialect = Dialect::sqlite;
    std::vector<SnapshotTable> tables;

    /// The database's views. SQLite only, and never diffed -- see
    /// `SnapshotView`.
    std::vector<SnapshotView> views;

    /// Whether this server stored table names folded to lower case.
    ///
    /// `lower_case_table_names` is non-zero by default on Windows and macOS
    /// and zero on Linux, so the same MySQL model is case-sensitive on one
    /// host and not on another. It is read rather than assumed, because
    /// guessing either way produces drift that never converges: a model
    /// declaring `OrderLine` against a folding server finds no `OrderLine`,
    /// proposes creating it on every run, and the healing run fails with
    /// "table 'orderline' already exists".
    ///
    /// Table names only. Both servers store column and index names with the
    /// case they were declared in, whatever this is set to -- and compare them
    /// without regard to it, which is why `SnapshotTable::find_column` and
    /// `find_index` fold ASCII case on every engine.
    ///
    /// Always true on SQLite, which keeps a table name's case and compares it
    /// without regard to ASCII case, so `ord_case` and `Ord_Case` are one table
    /// there on every host.
    bool folds_table_names = false;

    /// Whether this engine can drop a column in place.
    ///
    /// SQLite gained `ALTER TABLE ... DROP COLUMN` in 3.35, and the SQLite a
    /// distribution ships is often older: 3.34 on Debian 11 and RHEL 9. Where
    /// this is false the differ rebuilds the table without the column instead
    /// -- the same destructive change, made the long way round, and refused
    /// without the same opt-in. True for a snapshot built by hand.
    bool drops_columns_in_place = true;

    /// Whether this SQLite can create a `STRICT` table, which arrived in
    /// 3.37. Where it cannot, a model declaring one is refused rather than
    /// created without it. True for a snapshot built by hand.
    bool strict_tables = true;

    /// The name of the schema that was read, as the server reports it. MySQL
    /// family only; empty on SQLite and in a snapshot built by hand.
    ///
    /// A binding that names this very schema is a key into it, and is compared
    /// as one: the reader reports such a key with no `referenced_schema`.
    /// It follows every member that was there before it, so a brace
    /// initialiser written then still means what it meant.
    std::string schema{};

    /// Whether the session that read this reads a backslash in a string
    /// literal as an escape: MySQL and MariaDB do unless the session's
    /// `sql_mode` holds `NO_BACKSLASH_ESCAPES`. Read once per run, and what
    /// the model's CHECKs are read under, since the run sends them to the
    /// same session; the servers print a stored CHECK with the escapes either
    /// way. True in a snapshot built by hand, and on SQLite, where nothing
    /// asks: SQLite reads no escapes at all. Last among the members, so an
    /// initialiser written before it existed still means what it meant.
    bool backslash_escapes = true;

    /// Find a table, under whatever name-folding this server does.
    [[nodiscard]] const SnapshotTable* find(std::string_view wanted) const;

    /// Whether `a` and `b` name the same table on this server.
    [[nodiscard]] bool same_table_name(std::string_view a, std::string_view b) const;
};

/// Read the live schema through a connection.
///
/// Implemented entirely in terms of SQL over the abstract Connection --
/// `information_schema` on the MySQL family, `pragma_*` on SQLite -- so it
/// lives in sch_core rather than in an engine archive, and needs no vendor
/// header.
///
/// Schemata's own bookkeeping tables are excluded on every engine, and
/// SQLite's internal `sqlite_` tables on SQLite, so the differ never proposes
/// dropping them. Only SQLite reserves that prefix: on MySQL and MariaDB a
/// table called `sqlite_x` is an ordinary table and is read like one. Views
/// are excluded from `tables` too: a model describes tables, and a view is not
/// one -- listing them there would have the differ propose dropping every view
/// in the schema. SQLite's views are read into `views` instead, where nothing
/// diffs them and the one thing that reads them is the guard that refuses to
/// take a column out from under one. A MariaDB system-versioned table is a
/// table, and is read; a MariaDB sequence, which the server lists beside its
/// tables, is not.
///
/// `schema` reads another schema than the session's own, leaving the session
/// where it was -- see `DiffOptions::schema`. Naming one needs the connection
/// to have a default schema of its own to go back to.
///
/// On MySQL and MariaDB it costs a fixed number of statements whatever the
/// number of tables: one for the session's settings, one listing the tables,
/// and then each catalog read once for the whole schema -- the columns, the
/// indexes, the foreign keys (two), the CHECKs (two on MySQL, one on MariaDB)
/// and the keys other schemas hold onto this one's tables. Nine on MySQL and
/// eight on MariaDB, and two for a schema with no tables; naming another
/// schema adds the few that switch the session there and back. On SQLite it
/// is several statements a table. For the names alone, see `list_tables()`.
[[nodiscard]] SchemaSnapshot read_schema(driver::Connection& connection,
                                         std::string_view    schema = {});

/// The names of the tables `read_schema()` would read, and nothing else about
/// them, in one statement.
///
/// Exactly the names of `read_schema(connection, schema).tables`, in the same
/// order: base tables, a MariaDB system-versioned one included, and never a
/// view or a MariaDB sequence; Schemata's own bookkeeping tables
/// (`__schemata_*`) left out on every engine, and SQLite's internal `sqlite_`
/// tables on SQLite, where a virtual table and the shadow tables its module
/// keeps are listed as `read_schema()` reads them. Read from
/// `information_schema.TABLES` on the MySQL family and from `sqlite_master` on
/// SQLite.
///
/// `schema` lists another schema's tables than the session's own, MySQL and
/// MariaDB only, compared as the server compares database names. It is bound
/// as a value, never spliced into the statement, and the session is left
/// where it is -- so, unlike `read_schema()`, a connection with no default
/// schema of its own may name one. Empty is the session's own, and a session
/// with none has no tables. Allowed inside a transaction; it only reads.
///
/// \throws schema_error for a schema on SQLite, for one that cannot be an
///         identifier, and for one the server has no schema by -- each as
///         `read_schema()` refuses it.
[[nodiscard]] std::vector<std::string> list_tables(driver::Connection& connection,
                                                   std::string_view    schema = {});

/// The granularity at which two types can be told apart on this engine.
///
/// SQLite stores affinities, not types: `BOOLEAN`, `INT` and `BIGINT` are
/// indistinguishable once written, as are `TEXT`, `DATETIME` and `DATE`. A
/// differ that compared Schemata's own ColumnType there would report drift
/// forever on a database that is in fact correct.
///
/// Collapsing both sides through this function before comparing is what
/// makes a no-change run produce an empty diff.
[[nodiscard]] ColumnType comparable_type(ColumnType type, Dialect dialect);

/// The type a column was declared as, where the engine keeps only an affinity.
///
/// On SQLite `SnapshotColumn::type` is the affinity SQLite derives from the
/// declared type, which decides how a value is stored and not what the
/// application meant by the column. DATETIME, DATE, BOOLEAN and DECIMAL all
/// have NUMERIC affinity, and a column declared with no type at all has BLOB
/// affinity, which only means that nothing is converted. This reads the
/// declaration for what it says:
///
///     DATETIME, TIMESTAMP     timestamp
///     DATE                    date
///     BOOLEAN, BOOL           boolean
///     DECIMAL, NUMERIC        decimal
///     no declared type        text
///     any other NUMERIC one   text     -- TIME, JSON, UUID, MONEY, STRING, ANY
///
/// and leaves every type SQLite's own rules already name -- the integers, the
/// text types, BLOB, REAL, FLOAT and DOUBLE -- as the affinity says.
///
/// The scaffolder maps a column by this, and the differ accepts a model column
/// of this type as matching the live one. Refusing that would report drift
/// against the very database a model was scaffolded from, and the only heal on
/// offer is rebuilding the application's table to change nothing but its
/// declared types.
///
/// `column.type`, unchanged, on the MySQL family, which keeps the type itself,
/// and for a column whose declared type is not the one its affinity came from
/// -- a snapshot built by hand.
[[nodiscard]] ColumnType declared_column_type(const SnapshotColumn& column, Dialect dialect);

/// The granularity at which two referential actions can be told apart.
///
/// The same argument as `comparable_type`, for the same reason. InnoDB
/// implements RESTRICT and NO ACTION identically and the two servers do not
/// even agree on what to call the result: for one constraint written the
/// same way, MySQL reports NO ACTION and MariaDB reports RESTRICT. SQLite
/// does distinguish them, so only the MySQL family is folded.
///
/// Both sides must be passed through this before comparing. Folding only
/// the value read back would trade one permanent drift for another, since a
/// model that asks for RESTRICT explicitly would then never match.
[[nodiscard]] ReferentialAction comparable_action(ReferentialAction action, Dialect dialect);

/// The granularity at which two collation names can be told apart.
///
/// The third of these, and the same argument again. Collation names are
/// case-insensitive on all three engines, so the comparison folds case while
/// emission stays verbatim. And on SQLite a column with no `COLLATE` clause
/// is `BINARY`, so absent and `BINARY` have to compare equal or a model that
/// says `BINARY` drifts against the database that agrees with it.
///
/// What it deliberately does *not* do is relate one engine's names to
/// another's. `NOCASE` folds the 26 ASCII letters and nothing else, so
/// whether it agrees with `utf8mb4_0900_ai_ci` depends on what is in the
/// column -- a claim about the data that only its owner can make, and the
/// reason a model declares the correspondence rather than inheriting one.
[[nodiscard]] std::string comparable_collation(std::string_view name, Dialect dialect);

/// The prefix reserved for Schemata's bookkeeping tables.
inline constexpr std::string_view internal_table_prefix = "__schemata_";

} // namespace schemata
