#pragma once

#include <string>
#include <vector>

#include <schemata/schema/snapshot.h>

namespace schemata::scaffold {

/// Reverse-engineering a live schema into C++ entities.
///
/// Pure text production from a `SchemaSnapshot`: nothing here opens a
/// connection, so every naming rule, type mapping and relationship inference
/// is testable against a fixture with no database present. It also shares
/// the reader with the differ, so what is scaffolded and what is diffed can
/// never disagree about what the database contains.

struct Options {
    /// Namespace for the generated entities. Empty puts them at global scope.
    ///
    /// One identifier, or several joined by `::`. Refused by `emit()` if it is
    /// anything else: unlike every name read from the database, this one is
    /// written into the generated source as given, so a value that is not a
    /// namespace name produces a file that does not compile -- or, if it came
    /// from somewhere nobody typed it, one that compiles and does something
    /// else.
    std::string name_space = "model";

    /// Base name for the context file and its model factory.
    ///
    /// A plain C++ identifier, and refused by `emit()` otherwise, for the same
    /// reason and one more: it is also the file name, so it cannot be
    /// quietly corrected without writing somewhere the caller is not looking.
    std::string context = "Schema";

    /// Emit an `#include "<Entity>.custom.inl"` hook inside each struct.
    ///
    /// C++ has no partial classes, so there is no clean way to add a member
    /// to a generated type and keep regenerating it. The hook is the least
    /// bad option: re-scaffolding overwrites the generated file and leaves
    /// the .inl alone. Off by default, because an include of a file that
    /// does not exist is a build error, and most callers never edit.
    bool custom_hooks = false;
};

struct GeneratedFile {
    std::string path;
    std::string content;
};

/// Emit one header per entity plus one context header.
///
/// The entity headers hold plain structs. Every `describe()` lives in the
/// context header, which is not only EF Core's shape but the one that works
/// in C++: `belongs_to<Parent>` needs `Parent` to be a complete type, and
/// two entities that reference each other cannot both include the other.
/// Collecting the descriptions in one file that includes them all removes
/// the ordering problem entirely.
[[nodiscard]] std::vector<GeneratedFile> emit(const SchemaSnapshot& snapshot,
                                              const Options&        options = {});

/// What `emit()` leaves out of the model it writes, one line each.
///
/// Some of what a database holds has no place in a model and no effect on
/// what the model can do: a virtual table and the tables its module keeps,
/// an index over an expression, a column the server computes. Those are left
/// out, each with a comment in the generated code where it would have been,
/// rather than written in a form `validate()` refuses. This is the same list,
/// for a tool to say out loud what the generated code says quietly.
[[nodiscard]] std::vector<std::string> omissions(const SchemaSnapshot& snapshot);

// -- naming, exposed because the rules are worth testing directly -----------

/// Turn a table name into an entity name: `blog_posts` -> `BlogPost`.
///
/// Singularisation is a heuristic over English plurals and will get some
/// names wrong. That is tolerable for generated code a human reads once and
/// renames, and the alternative -- a dictionary -- is worse.
[[nodiscard]] std::string entity_name(std::string_view table);

/// Make a C++ identifier out of a database name.
///
/// Characters that cannot appear in an identifier become underscores, a
/// leading digit gains one, and a name that collides with a keyword or with
/// a macro likely to be in scope gains a trailing one. The database name is
/// still what `describe()` emits, so the mapping stays correct however the
/// member ends up spelled.
///
/// The macro half cannot be complete -- a macro is whatever the consumer
/// defined -- so it covers every object-like macro of `<cerrno>`,
/// `<climits>`, `<cstdint>`, `<cstdio>` and `<cstdlib>`, the common ones of
/// `<windows.h>`, and the compilers' non-strict predefines, which is where the
/// collisions that actually happen come from. Identical on every platform, so
/// the generated header is the same file wherever it was produced.
[[nodiscard]] std::string identifier(std::string_view name);

/// The C++ type for a column, including `std::optional` when nullable.
///
/// Maps `column.type` as given. On SQLite that is the column's affinity, which
/// is not what `emit()` maps: it reads the declared type first, through
/// `declared_column_type`, so a `DATETIME` column becomes a timestamp rather
/// than the double its NUMERIC affinity would make it.
[[nodiscard]] std::string cpp_type(const SnapshotColumn& column);

} // namespace schemata::scaffold
