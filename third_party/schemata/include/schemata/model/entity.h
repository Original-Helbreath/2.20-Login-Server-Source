#pragma once

#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <schemata/core/converter.h>
#include <schemata/core/identifier.h>
#include <schemata/model/model.h>

namespace schemata {

/// How an entity maps to a table.
///
/// Specialise this for each entity and provide a static `describe()`:
///
///     template <> struct schemata::Entity<User> {
///         static void describe(schemata::EntityBuilder<User>& e) {
///             e.table("users");
///             e.key("id", &User::id).identity();
///             e.prop("email", &User::email).max_length(255).unique();
///         }
///     };
///
/// Column names are passed explicitly because C++20 has no reflection and a
/// member pointer carries no name. `SCH_KEY` and `SCH_PROP` below remove the
/// duplication for the common case where the column matches the member.
///
/// The primary template is undefined, so using an unmapped type is a compile
/// error naming that type.
template <typename T>
struct Entity;

// ---------------------------------------------------------------------------

/// Chainable configuration for one column.
///
/// Holds a table pointer and an index rather than a column reference,
/// because adding the next column may reallocate the column vector.
class ColumnBuilder {
public:
    ColumnBuilder(Table& table, std::size_t index) noexcept
        : table_(&table), index_(index) {}

    /// Override the column name set at declaration.
    ColumnBuilder& column(std::string name) {
        col().name = std::move(name);
        return *this;
    }

    /// The longest value this column holds.
    ///
    /// For text and blob columns. The engine picks the narrowest type that
    /// holds it: `VARCHAR(n)` up to `max_varchar_length` characters, then
    /// `TEXT`, `MEDIUMTEXT` or `LONGTEXT`; and for a blob `VARBINARY(n)` up
    /// to `max_varbinary_length` bytes, then `BLOB`, `MEDIUMBLOB` or
    /// `LONGBLOB`. Absent renders as `TEXT` or `BLOB`.
    ///
    /// The two cut-overs are the same 65535-byte row seen twice: 16383
    /// characters at utf8mb4's four bytes each is 65532 bytes, and three
    /// bytes go to the length prefix and the null flag.
    ///
    /// Only enforced, and only compared, on the MySQL family. SQLite gives
    /// every textual column TEXT affinity whatever length is declared, so a
    /// length that changes there produces no statement and no drift -- the
    /// declared type in the stored DDL simply stays as it was first written.
    /// Treat it as documentation of intent on SQLite and as a constraint on
    /// the other two.
    ///
    /// Ignored entirely alongside `one_of()` on the MySQL family, where the
    /// values are the type: `ENUM` declares no length.
    ColumnBuilder& max_length(int length) {
        col().max_length = length;
        return *this;
    }

    /// Store exactly `max_length`, padding rather than trimming.
    ///
    /// `CHAR(n)` rather than `VARCHAR(n)`, `BINARY(n)` rather than
    /// `VARBINARY(n)`. Worth it for a column that genuinely is fixed -- a
    /// hex digest, a currency code -- where the length saves the per-value
    /// length prefix and says what the column means.
    ///
    /// Only the MySQL family pads. SQLite keeps what it was given whatever
    /// the declared type says, which is a divergence rather than a bug: see
    /// DESIGN.md.
    ColumnBuilder& fixed_length() {
        col().fixed_length = true;
        return *this;
    }

    /// Store this column as an exact `DECIMAL(precision, scale)`.
    ///
    /// For a `double` member whose values are decimal quantities rather than
    /// measurements -- money, percentages, rates -- where binary rounding is
    /// visible to whoever reads the total. The engine then holds the digits
    /// rather than the nearest binary fraction to them.
    ///
    /// `precision` is total digits and `scale` is how many of those fall
    /// after the point, so `.decimal(5, 2)` holds up to 999.99. The value
    /// still travels through a `double`, which round-trips up to
    /// `max_exact_decimal_digits` exactly; `validate()` refuses more rather
    /// than storing a number that cannot come back.
    ///
    /// **Not enforced on SQLite, and this one costs money.** SQLite has
    /// affinities rather than types: the column is spelled `DECIMAL(p,s)` and
    /// stored as given, so nothing is rounded to `scale` and nothing is
    /// range-checked against `precision`. The same model therefore stores
    /// different values on different engines, with no error on either side:
    ///
    ///     .decimal(12, 4)   1.00005  ->  mysql 1.0001     sqlite 1.00005
    ///     .decimal(15, 0)   2.5      ->  mysql 3          sqlite 2.5
    ///     .decimal(12, 4)   1.0e9    ->  mysql *refused*  sqlite 1000000000
    ///
    /// A team that develops against SQLite and deploys to the MySQL family
    /// will see totals change at the boundary, and a value SQLite accepted in
    /// testing is rejected in production with error 1264.
    ///
    /// Schemata does not round on SQLite's behalf. Doing so would make the
    /// library rewrite a value the caller supplied, on one engine only, to
    /// match another engine's rules -- a claim about the data that only its
    /// owner can make. If exact agreement across engines matters more than
    /// the decimal spelling, store minor units in an integer column instead:
    /// that means the same thing everywhere, which is the only guarantee
    /// worth relying on.
    ColumnBuilder& decimal(int precision, int scale) {
        col().type      = ColumnType::decimal;
        col().precision = precision;
        col().scale     = scale;
        return *this;
    }

    /// Fractional-second digits for a timestamp column, 0 to 6.
    ///
    /// Defaults to 6, which keeps everything `schemata::timestamp` holds.
    /// Declaring fewer asks the MySQL family to round on the way in. SQLite
    /// stores a timestamp as text and ignores the declared precision, which
    /// is a divergence rather than something to emulate: see DESIGN.md.
    ///
    /// A value written by hand keeps whatever it was given. What the
    /// `on_update_now()` trigger writes for itself is three digits, not six:
    /// `strftime` offers `%f` and nothing finer.
    ColumnBuilder& precision(int digits) {
        col().precision = digits;
        return *this;
    }

    /// Accept only these values, and nothing else.
    ///
    /// `ENUM('a','b')` on the MySQL family, which stores the position rather
    /// than the text and so costs one or two bytes whatever the values are.
    /// SQLite has no enumeration, so it gets a `VARCHAR` long enough for the
    /// longest value with a `CHECK ... IN` beside it -- a different type name
    /// accepting exactly the same set.
    ///
    /// For a `std::string` member. The C++ side stays a string rather than
    /// becoming a generated enum: an enumeration in the database is a set of
    /// values, and a row written before a value was removed still holds it.
    ///
    /// An enumeration's values *are* its type, so they live in the schema
    /// metadata rather than in a column -- and the MySQL family describes its
    /// own metadata in utf8mb3. A value holding a character outside the Basic
    /// Multilingual Plane goes in and reads back as `?`, so the column would
    /// drift against the model for ever. `validate()` refuses those there
    /// rather than letting the schema reach a state no run can settle. The
    /// restriction is on the enumeration's *values*, not on what a text
    /// column may hold: an ordinary column stores them and returns them
    /// unchanged.
    ColumnBuilder& one_of(std::vector<std::string> values) {
        col().allowed = std::move(values);
        return *this;
    }

    /// Set this column to the current time whenever the row is updated.
    ///
    /// `ON UPDATE CURRENT_TIMESTAMP` on the MySQL family. SQLite has no such
    /// clause, so it gets an `AFTER UPDATE` trigger instead.
    ///
    /// The two agree where it matters most: a statement that sets the column
    /// itself keeps the value it set. They differ on one case. MySQL and
    /// MariaDB move the column only when the UPDATE actually changes a value
    /// in the row; the trigger fires on any UPDATE that leaves this column as
    /// it was, including one that writes every value back unchanged. So
    /// `UPDATE t SET title = title` moves the timestamp on SQLite and leaves it
    /// alone on the servers.
    ///
    /// That is left as it is on purpose. Closing it would mean a trigger that
    /// compares every other column, and the reader recognises the library's
    /// trigger by its text -- a new text would read every existing trigger in
    /// every database as somebody else's and replace it.
    ColumnBuilder& on_update_now() {
        col().on_update_now = true;
        return *this;
    }

    /// Compare and sort this column under the named collation.
    ///
    /// `alias` is a name the model chose, not an engine's -- the three
    /// engines share no collation names and the correspondence between them
    /// cannot be guessed, so `Model::collation` declares separately what each
    /// one calls it. A model that names a collation it has not mapped for the
    /// dialect being used is refused by `validate()`.
    ///
    /// Leaving this off is not the same as asking for the default: a column
    /// that says nothing is not compared, and keeps whatever collation the
    /// database already gave it -- unless the model gives a default
    /// (`Model::default_collation`), which a text column that says nothing is
    /// then held to.
    ColumnBuilder& collation(std::string alias) {
        col().collation = std::move(alias);
        return *this;
    }

    /// Rename this column from `old_name` in place, keeping its rows.
    ///
    /// `SCH_PROP(e, value).renamed_from("sell_price")`: where the live table
    /// still has `sell_price` and no `value`, the next `ensure_schema()` runs
    /// `ALTER TABLE ... RENAME COLUMN`, with no rebuild and no data lost, and
    /// classifies it additive. Anything else about the column the model
    /// changes in the same release -- its type, its CHECK -- is diffed against
    /// the renamed column and applied after the rename as it would be on any
    /// other. Safe to leave in the model: once the rename has run it is a
    /// no-op. See `Column::renamed_from`.
    ColumnBuilder& renamed_from(std::string old_name) {
        col().renamed_from = std::move(old_name);
        return *this;
    }

    ColumnBuilder& unique() {
        col().unique = true;
        return *this;
    }

    /// Generated by the database on insert; read back rather than written.
    ColumnBuilder& identity() {
        col().identity = true;
        return *this;
    }

    /// SQLite only: write this identity key as `INTEGER PRIMARY KEY
    /// AUTOINCREMENT`, so a key is never reused after the highest row is
    /// deleted. Off by default -- see `Column::autoincrement`.
    ColumnBuilder& autoincrement() {
        col().identity      = true;
        col().autoincrement = true;
        return *this;
    }

    /// Raw SQL for the column default, emitted verbatim into DDL.
    ///
    /// Part of the schema rather than a value, so it comes from the model
    /// and is never a bound parameter.
    ///
    /// A default never applies to a row Schemata inserts. Its INSERT names
    /// every mapped column but a generated key, so an empty `std::optional`
    /// is written as NULL and a plain member as whatever it holds; the
    /// default fills rows that other code inserts, and the rows already there
    /// when the column is added. Leaving an empty optional out of the INSERT
    /// instead would make NULL impossible to insert into a nullable column
    /// that has a default. Initialise the member to the same value if a
    /// Schemata insert should carry it too.
    ColumnBuilder& default_sql(std::string sql) {
        col().default_sql = std::move(sql);
        return *this;
    }

    /// Constrain what this column may hold, as raw SQL.
    ///
    /// The expression alone, without the keyword or its parentheses, and
    /// naming the column unquoted so that it reads the same on all three
    /// engines:
    ///
    ///     b.property(&Character::level, "level").check("level >= 1");
    ///
    /// One per column. Anything needing two is one expression with `AND`,
    /// and anything naming a second column is `EntityBuilder::check`.
    ///
    /// Not a substitute for `one_of`, which says the same thing about a
    /// string column and says it as an `ENUM` where an engine has one. This
    /// is for what no other clause here can express.
    ColumnBuilder& check(std::string expression) {
        col().check = std::move(expression);
        return *this;
    }

    /// Create a non-unique index over this column alone.
    ColumnBuilder& indexed(std::string name = {}) {
        if (name.empty()) {
            name = generated_identifier(std::format("IX_{}_{}", table_->name, col().name),
                                        Dialect::mysql);
        }
        table_->indexes.push_back(Index{std::move(name), {col().name}, false});
        return *this;
    }

private:
    [[nodiscard]] Column& col() const { return table_->columns[index_]; }

    Table*      table_;
    std::size_t index_;
};

/// Chainable configuration for one foreign key.
class ForeignKeyBuilder {
public:
    ForeignKeyBuilder(Table& table, std::size_t index) noexcept
        : table_(&table), index_(index) {}

    ForeignKeyBuilder& on_delete(ReferentialAction action) {
        fk().on_delete = action;
        return *this;
    }

    ForeignKeyBuilder& on_update(ReferentialAction action) {
        fk().on_update = action;
        return *this;
    }

    ForeignKeyBuilder& constraint_name(std::string name) {
        fk().name = std::move(name);
        return *this;
    }

private:
    [[nodiscard]] ForeignKey& fk() const { return table_->foreign_keys[index_]; }

    Table*      table_;
    std::size_t index_;
};

// ---------------------------------------------------------------------------

/// The object handed to `Entity<T>::describe()`.
template <typename T>
class EntityBuilder {
public:
    using entity_type = T;

    explicit EntityBuilder(Table& table) noexcept : table_(&table) {}

    /// Set the table name. Defaults to the entity's type name when unset.
    void table(std::string name) { table_->name = std::move(name); }

    /// Make the table `STRICT` on SQLite, so a value of the wrong type is
    /// refused rather than stored. See `Table::strict`.
    ///
    /// A STRICT column may only be declared INTEGER, REAL, TEXT, BLOB or ANY,
    /// so a length or a DECIMAL's digits are not written into the table:
    /// SQLite enforced neither anyway, and the model still carries both.
    void strict() { table_->strict = true; }

    /// Map a member to a column.
    template <typename M>
        requires Storable<M>
    ColumnBuilder prop(std::string name, M T::*ptr) {
        return add_column(std::move(name), ptr, false);
    }

    /// Map a member to a primary key column.
    ///
    /// Called more than once to declare a composite key; the columns form
    /// the key in declaration order.
    ///
    /// **A composite key gives up every relationship.** The table itself is
    /// fully usable -- DDL, insert, update, delete, querying, ordering,
    /// paging and scaffolding all work exactly as they do for a single-column
    /// key -- but nothing may reference it and nothing may navigate from it:
    ///
    ///   - `find()` takes a single-column key and says so;
    ///   - `belongs_to()` takes one member pointer, so it cannot express a
    ///     reference to a two-column key, and `validate()` refuses the
    ///     malformed foreign key it would produce;
    ///   - `has_many()` cannot target a composite-keyed parent, because
    ///     `include()` follows a single-column key.
    ///
    /// Each of those is refused at `validate()` with a message naming it, so
    /// nothing here fails quietly or late -- but the last one refuses the
    /// whole model, which means `ensure_schema()` will not create the tables
    /// either. Declaring a composite key is therefore a decision about the
    /// entity's relationships as much as about its identity. A surrogate
    /// single-column key with a unique index over the natural columns keeps
    /// both.
    template <typename M>
        requires Storable<M>
    ColumnBuilder key(std::string name, M T::*ptr) {
        return add_column(std::move(name), ptr, true);
    }

    /// Declare that a column references another entity's primary key.
    ///
    /// The foreign key is declared on the dependent side, which is where it
    /// physically lives. The referenced entity does not need to be
    /// registered yet: targets are resolved once every entity is added, so
    /// declaration order does not matter and cycles are fine.
    template <typename Referenced, typename M>
        requires Storable<M>
    ForeignKeyBuilder belongs_to(M T::*ptr) {
        const auto* column = table_->find_member(member_key(ptr));
        if (column == nullptr) {
            throw std::logic_error(std::format(
                "schemata: belongs_to on table '{}' names a member that is not mapped; "
                "declare it with prop() first",
                table_->name));
        }

        ForeignKey fk;
        fk.columns            = {column->name};
        fk.referenced_entity  = std::type_index(typeid(Referenced));
        table_->foreign_keys.push_back(std::move(fk));
        return ForeignKeyBuilder{*table_, table_->foreign_keys.size() - 1};
    }

    /// Declare that columns reference a table in another schema on the same
    /// server -- MySQL only -- or, with an empty `schema`, a table of this
    /// model by name, which is how a composite key is declared on any engine:
    /// `belongs_to` takes one member and cannot say one.
    ///
    ///     e.references({"item_id"}, "world", "items", {"id"})
    ///         .on_delete(schemata::ReferentialAction::cascade);
    ///
    ///     e.references({"character_id", "account_id"},
    ///                  "player", "characters", {"id", "account_id"});
    ///
    /// `schema` is an alias this model chooses, not the schema's name: the
    /// name belongs to the environment and is supplied when the schema is
    /// ensured, as `DiffOptions::schema_bindings` -- `{"world", "hb_dev_world"}`
    /// -- so that one model fingerprints the same in every environment it
    /// runs in. An alias nobody bound is refused, naming it, rather than read
    /// as this schema.
    ///
    /// Columns are named rather than given as members, because a key into
    /// another schema has no entity type on this side to resolve anything
    /// from, and because it may be composite. Each column must be mapped with
    /// `prop()` or `key()`; the referenced ones must be the other table's
    /// primary key or a unique index over exactly those columns, which is what
    /// MySQL 8.4 requires. The constraint is named as `belongs_to` names one
    /// unless `constraint_name()` says otherwise.
    ForeignKeyBuilder references(std::vector<std::string> columns, std::string schema,
                                 std::string table, std::vector<std::string> referenced_columns) {
        ForeignKey fk;
        fk.columns            = std::move(columns);
        fk.referenced_schema  = std::move(schema);
        fk.referenced_table   = std::move(table);
        fk.referenced_columns = std::move(referenced_columns);
        // Named once the entity is added, as a belongs_to key is, so the name
        // carries the table's name whether e.table() came first or not.
        table_->foreign_keys.push_back(std::move(fk));
        return ForeignKeyBuilder{*table_, table_->foreign_keys.size() - 1};
    }

    /// Declare a collection navigation: the many side of a one-to-many.
    ///
    /// `&User::posts` is a `std::vector<Post>` that `describe()` never maps
    /// as a column, and `&Post::author_id` names the foreign key linking the
    /// two. Nothing here reaches the schema -- the key itself is still
    /// declared with `belongs_to` on the dependent side -- and the vector
    /// stays empty unless a query asks for it with `include()` (D3). There
    /// is no lazy loading, so an untouched navigation costs nothing and
    /// never surprises anyone with a statement they did not write.
    ///
    /// The related entity does not have to be registered yet; it is looked
    /// up when a query includes the navigation.
    template <typename Child, typename K>
    void has_many(std::string name, std::vector<Child> T::*ptr, K Child::*fk) {
        Navigation nav;
        nav.name        = std::move(name);
        nav.member      = member_key(ptr);
        nav.target      = std::type_index(typeid(Child));
        nav.foreign_key = member_key(fk);
        nav.collection  = true;

        nav.clear  = [ptr](void* owner) { (static_cast<T*>(owner)->*ptr).clear(); };
        nav.attach = [ptr](void* owner, const Table& target, const driver::Statement& row) {
            Child child{};
            detail::materialise_row(target, row, &child);
            (static_cast<T*>(owner)->*ptr).push_back(std::move(child));
        };

        add_navigation(std::move(nav));
    }

    /// Declare a reference navigation: the one side of a one-to-many.
    ///
    /// Spelled `std::optional<User>` rather than a plain `User`, because
    /// otherwise a navigation nobody loaded and a related row full of
    /// defaults would be the same object. The foreign key is on this entity,
    /// which is the dependent side.
    template <typename Other, typename K>
    void has_one(std::string name, std::optional<Other> T::*ptr, K T::*fk) {
        Navigation nav;
        nav.name        = std::move(name);
        nav.member      = member_key(ptr);
        nav.target      = std::type_index(typeid(Other));
        nav.foreign_key = member_key(fk);
        nav.collection  = false;

        nav.clear  = [ptr](void* owner) { (static_cast<T*>(owner)->*ptr).reset(); };
        nav.attach = [ptr](void* owner, const Table& target, const driver::Statement& row) {
            Other value{};
            detail::materialise_row(target, row, &value);
            static_cast<T*>(owner)->*ptr = std::move(value);
        };

        add_navigation(std::move(nav));
    }

    /// Constrain the row rather than one column, as raw SQL.
    ///
    /// For what a column's own `check` cannot say, which is anything naming
    /// a second column:
    ///
    ///     b.check("ck_booking_span", "ends_at IS NULL OR ends_at >= starts_at");
    ///
    /// The name is emitted so a server can report the constraint back under
    /// it. It is not how the constraint is recognised -- see `Column::check`
    /// -- so renaming one does not rebuild the table.
    void check(std::string name, std::string expression) {
        table_->checks.push_back(TableCheck{std::move(name), std::move(expression)});
    }

    /// Say that a CHECK the table may still hold is gone, by the constraint's
    /// name or by its expression:
    ///
    ///     b.retire_check("id >= 1");
    ///     b.retire_check("ck_booking_span");
    ///
    /// A CHECK the database has and the model does not declare is otherwise
    /// left alone -- it may be a rule the database keeps for reasons of its
    /// own -- so taking one away has to be said. `ensure_schema()` then drops
    /// it: `DROP CHECK` on MySQL, `DROP CONSTRAINT` on MariaDB, and a rebuild
    /// on SQLite, which keeps every row and does not count the CHECK as lost.
    /// Relaxing a rule is widening, not destructive. `verify_schema()` reports
    /// it until it is gone; after that the line does nothing, and may stay.
    ///
    /// An expression compares as the differ compares CHECKs, so spacing and
    /// the engine's own rewriting do not matter. A rule the model still
    /// declares is kept whatever this says.
    void retire_check(std::string name_or_expression) {
        if (name_or_expression.empty()) {
            throw std::logic_error(std::format(
                "schemata: retire_check() on table '{}' needs a constraint name or an "
                "expression",
                table_->name));
        }
        table_->retired_checks.push_back(std::move(name_or_expression));
    }

    /// Create an index over one or more columns, named automatically when no
    /// name is given.
    void index(std::vector<std::string> columns, bool unique = false,
               std::string name = {}) {
        if (name.empty()) {
            name = unique ? "UX_" : "IX_";
            name += table_->name;
            for (const auto& c : columns) {
                name += "_";
                name += c;
            }
            name = generated_identifier(std::move(name), Dialect::mysql);
        }
        table_->indexes.push_back(Index{std::move(name), std::move(columns), unique});
    }

private:
    void add_navigation(Navigation nav) {
        if (table_->find_member(nav.member) != nullptr) {
            throw std::logic_error(std::format(
                "schemata: '{}' on table '{}' is mapped as a column as well as a "
                "navigation; a navigation is not stored and must not be a column",
                nav.name, table_->name));
        }
        for (const Navigation& existing : table_->navigations) {
            if (existing.member == nav.member || existing.name == nav.name) {
                throw std::logic_error(std::format(
                    "schemata: table '{}' declares the navigation '{}' twice",
                    table_->name, nav.name));
            }
        }
        table_->navigations.push_back(std::move(nav));
    }

    template <typename M>
    ColumnBuilder add_column(std::string name, M T::*ptr, bool is_key) {
        using Conv = ValueConverter<M>;

        Column c;
        c.name        = std::move(name);
        c.type        = Conv::column_type();
        c.nullable    = Conv::nullable();
        c.primary_key = is_key;
        c.member      = member_key(ptr);

        c.get = [ptr](const void* obj) -> Value {
            return Conv::to_value(static_cast<const T*>(obj)->*ptr);
        };
        c.set = [ptr](void* obj, const Value& v) {
            Conv::from_value(v, static_cast<T*>(obj)->*ptr);
        };

        table_->columns.push_back(std::move(c));
        return ColumnBuilder{*table_, table_->columns.size() - 1};
    }

    Table* table_;
};

/// Satisfied when `T` has an `Entity<T>` specialisation with a `describe()`.
template <typename T>
concept MappedEntity = requires(EntityBuilder<T>& builder) {
    Entity<T>::describe(builder);
};

// ---------------------------------------------------------------------------

template <typename T>
Model& Model::add() {
    static_assert(std::is_default_constructible_v<T>,
                  "schemata: a mapped entity must be default-constructible; the "
                  "materialiser constructs one per row, and offsets are measured "
                  "against a probe instance");

    // Adding twice is a no-op so that a context naming both sides of a
    // relationship need not track what it has already registered.
    if (find<T>() != nullptr) {
        return *this;
    }

    // Built aside and moved in, so a describe() that throws leaves no
    // half-populated table behind.
    Table table;
    table.entity = std::type_index(typeid(T));

    EntityBuilder<T> builder{table};
    Entity<T>::describe(builder);

    if (table.name.empty()) {
        throw std::logic_error(
            "schemata: describe() did not name a table; call e.table(\"...\"). "
            "There is no default, because C++ type names are mangled and would "
            "make the schema depend on the compiler");
    }

    tables_.push_back(std::move(table));
    resolve_references();
    return *this;
}

} // namespace schemata

/// Declare a primary key column whose name matches the member.
#define SCH_KEY(builder, member_name)                                          \
    (builder).key(#member_name,                                                \
                  &std::remove_cvref_t<decltype(builder)>::entity_type::member_name)

/// Declare a column whose name matches the member.
#define SCH_PROP(builder, member_name)                                         \
    (builder).prop(#member_name,                                               \
                   &std::remove_cvref_t<decltype(builder)>::entity_type::member_name)
