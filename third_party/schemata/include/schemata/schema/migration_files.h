#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <schemata/schema/migrate.h>
#include <schemata/sql/dialect.h>

namespace schemata {

/// How `migrations_from_directory()` hashes a file's bytes into
/// `Migration::content_hash`.
enum class FileHash {
    /// Schemata's own 64-bit FNV-1a -- the hash every fingerprint and content
    /// hash the library records is taken with, which is not the standard
    /// FNV-1a 64 (see `<schemata/core/hash.h>`). The default: a project new to
    /// loading its migrations from files records what the rest of Schemata
    /// would.
    schemata,

    /// The standard FNV-1a 64 of the bytes -- offset basis
    /// 14695981039346656037, prime 1099511628211 -- as sixteen lowercase hex
    /// digits: `""` is `cbf29ce484222325`, `"a"` is `af63dc4c8601ec8c`. For a
    /// project whose history was recorded by a loader of its own that hashed
    /// its files that way.
    fnv1a64,
};

/// How `migrations_from_directory()` reads a directory.
struct DirectoryOptions {
    /// The header directive whose value becomes `Migration::note`: `"release"`
    /// reads `-- release: 1.4.0` as the note `1.4.0`. Matched as the other
    /// directives are, ignoring case. Empty reads no note; `phase` and
    /// `baseline`, in any case, are the other two directives, and refused with
    /// `migration_error` as the directory is listed.
    std::string note_directive{};

    /// The phase of a file that declares none with `-- phase:`.
    MigrationPhase default_phase = MigrationPhase::any;

    /// The hash of a file's bytes recorded as its `content_hash`.
    FileHash hash = FileHash::schemata;

    /// A hash of one's own, used instead of `hash` when set: given a file's
    /// bytes, it returns what to record. Called once when the directory is
    /// listed and again whenever the file is loaded to run, so it must return
    /// the same for the same bytes.
    std::function<std::string(std::string_view bytes)> hash_function{};
};

/// The migrations a directory of `.sql` files holds, one per file, ready for
/// `migrate()`.
///
/// **Which files.** Every regular file in `directory` itself -- not its
/// subdirectories -- whose extension is `.sql`, in any case; any other file
/// is ignored, a README beside the scripts say. A `.sql` entry that is a
/// symbolic link to nothing is refused, named, rather than skipped. A `.sql`
/// file's name must be digits, an underscore and a name holding no dot --
/// `^[0-9]+_[^.]+\.sql$`, `0004_seed_world_content.sql` -- or it is refused,
/// named. The id is the name without `.sql` (`0004_seed_world_content`), the
/// description what follows the first underscore. Both are the name's UTF-8
/// on every platform -- on Windows too, where a path's `string()` is the ANSI
/// code page's -- and so is `origin`, and every path a message names; a path
/// that is no UTF-8 -- a POSIX name is whatever bytes it was given -- is
/// refused, its bytes shown escaped. An id is the name's bytes, not
/// Unicode-normalized: a name spelled in NFD records another id than the same
/// name in NFC. Two files whose leading numbers are one number -- `0004_a.sql`
/// and `4_b.sql` too -- are refused, both named.
///
/// **In what order.** By the leading number, as a number. `migrate()` orders
/// ids as text, so a directory whose numbers and ids order differently --
/// `999_x` and `1000_y` -- is refused, naming the two: give the numbers one
/// width, `0999` and `1000`.
///
/// **The header.** A file may open with a block of lines that each start with
/// `--`, blank lines among them allowed; it ends at the first line that is
/// neither. A UTF-8 byte order mark before it is skipped; a file that begins
/// with a UTF-16 one is refused, named. Each line of the block is read as
/// `-- key: value`, or `-- key` for one that takes no value, the key and value
/// trimmed and the key compared ignoring case; three keys are directives:
///
/// - `-- phase: <value>`: `any`, `before_schema`, `after_columns` or
///   `after_schema`, ignoring case, as `Migration::phase`. Any other value is
///   refused, naming the file and the four. A file without one takes
///   `DirectoryOptions::default_phase`.
/// - `-- baseline`: the file is a baseline -- see below. It takes no value.
/// - `-- <note_directive>: <value>`: the value is `Migration::note`, where
///   `DirectoryOptions::note_directive` names one.
///
/// Any other line of the block is a comment, and ignored: a header is written
/// for people first. A directive given twice in one file is refused. Nothing
/// past the block is read as a directive.
///
/// The `--` is followed by a space, a tab or the end of the line, on every
/// dialect: MySQL and MariaDB read `--` as a comment only so, and a line such
/// as `--phase: any` would reach the server as the start of the first
/// statement, to be refused there. A header line that spells a directive with
/// no space after `--` is refused, naming the file. Any other such line --
/// `--note`, `-------` -- is a comment on SQLite, which reads `--` alone as
/// one, and is refused on the MySQL family, naming the file, for the reason
/// above. Both refusals hold only for a file from the newest baseline onward
/// -- the files returned, which could run; an older file sends nothing, and is
/// not refused for a line no server will read. A `--baseline` with no space in
/// an older file is safe to pass over: a newer file is the baseline, so that
/// one could not have been the newest. An older file is still refused for an
/// unknown phase, a directive given twice or a UTF-16 byte order mark: those
/// are faults in what the file says, not in how one server reads it.
///
/// **Baselines.** A project may squash its history into a baseline: a file
/// that states in full what the files before it built up, marked `--
/// baseline`. A baseline is a **new** file, numbered after every file any
/// database has run. One numbered lower sorts before a migration that
/// database recorded, and is refused as out of order: the ids a database
/// recorded anchor the order rule whether or not the list still holds them.
/// An existing file turned into a baseline -- the header line added to a file
/// that has run -- has changed, and is refused as changed.
///
/// Only the newest baseline and the files after it are returned; the older
/// ones are listed, their headers checked and their names held to the rules
/// above, but not read further or returned -- `list_migration_directory()`
/// returns their ids beside the list, as `superseded`. `migrate()` takes such
/// a list as
/// it promises to: a recorded migration the list leaves out is not run, not
/// compared and not refused -- it still anchors the order rule, and the
/// baseline sorts after every file it replaced. So on a database that
/// ran the replaced files, the baseline has never run, and **runs**: it is
/// new to that database as to a new one. Write it to be harmless where what
/// it states is already there -- `CREATE TABLE IF NOT EXISTS`, `INSERT ...
/// ON DUPLICATE KEY UPDATE`, `INSERT OR REPLACE` -- or keep a baseline to what
/// no database had yet. A baseline that has run is compared with what it was,
/// as any migration is, so a baseline is never edited once shipped: squash
/// again into a newer one.
///
/// **A superseded file stays on disk** where a deleted migration is to be
/// noticed. A baseline makes the files before it unnecessary to a new
/// database, and deleting them looks like tidying up; but to `migrate()` a
/// database that ran a file since deleted and one that ran a file a baseline
/// replaced are the same thing, a recorded id the list leaves out. What tells
/// them apart is the file still being there. `list_migration_directory()`
/// reports the replaced files' ids as `DirectoryListing::superseded`; passed
/// as `MigrateOptions::known_ids` with `MigrateOptions::refuse_missing` set,
/// a recorded id that no file in the directory names -- before the baseline
/// or after it -- is refused as `MigrationRefusal::missing`. Delete a
/// superseded file under that check and every database that ran it is refused:
/// the directory is append-only, a baseline included. This function returns
/// the list alone, which is all a caller that does not ask for the check
/// needs.
///
/// **What each migration holds.** `origin` is the file's path, `directory`
/// joined with the file's name. `content_hash` is the hash of the file's
/// bytes, read in binary, taken as `DirectoryOptions` says. `statements` is
/// empty: `load_statements` reads the file again when the migration has to
/// run, refuses it with `migration_error` if its bytes no longer hash to what
/// they did when the directory was listed -- "changed on disk since it was
/// listed" -- and otherwise splits it with `split_script(bytes, dialect)`,
/// the byte order mark left out. A file holding only comments has no
/// statements, and is recorded as run having run nothing.
///
/// Nothing is sent to a database.
///
/// \throws migration_error when `directory` is not a directory, a file in it
///         cannot be read, a `.sql` entry is a symbolic link to nothing or
///         one whose kind cannot be read, a name has no UTF-8 (half a
///         surrogate pair on Windows, bytes that are none on POSIX), or
///         `note_directive` is `phase` or `baseline`. What
///         `DirectoryOptions::hash_function` throws is thrown as it is.
/// \throws migration_history_error (a `schema_error`) for a `.sql` file named
///         outside the pattern, an unknown phase, a directive given twice or a
///         `baseline` given a value, a header line with no space after its
///         `--` (on SQLite only where it spells a directive; in a file from
///         the newest baseline onward), and a file that begins with a UTF-16
///         byte order mark (`MigrationRefusal::malformed`); two files sharing
///         a number (`MigrationRefusal::duplicate`); and numbers that order
///         the files otherwise than their ids (`malformed`); each naming the
///         file.
[[nodiscard]] std::vector<Migration> migrations_from_directory(
    const std::filesystem::path& directory, Dialect dialect,
    const DirectoryOptions& options = {});

/// What a directory of migration files holds: the list to run, and the ids of
/// the files a baseline took out of it.
struct DirectoryListing {
    /// What `migrations_from_directory()` returns: the newest baseline and
    /// the files after it, in order, or every file where none is a baseline.
    std::vector<Migration> migrations;

    /// The ids of the files before the newest baseline, in order: the
    /// migrations it replaced, whose files are still in the directory. Empty
    /// where no file is a baseline, or the first is. With two baselines it
    /// holds everything before the newer -- the older baseline too.
    ///
    /// For `MigrateOptions::known_ids`. A database that ran these has them on
    /// record, and `migrations` names none of them, so under
    /// `MigrateOptions::refuse_missing` they would each read as a file
    /// deleted. Here they are vouched for by the only thing that can: the
    /// file being there.
    std::vector<std::string> superseded;
};

/// `migrations_from_directory()`, with the ids of the files it leaves out.
///
/// The same listing, held to the same rules and refused for the same faults,
/// by the same exceptions -- whose messages name `migrations_from_directory()`
/// whichever of the two was called; `migrations_from_directory()` is this
/// call's `migrations`. What this adds is `superseded`, which that call works
/// out and drops.
///
/// It is what lets a deleted migration file be told from a superseded one:
///
///     const auto listing = list_migration_directory(dir, db->dialect());
///     MigrateOptions options;
///     options.refuse_missing = true;
///     options.known_ids      = listing.superseded;
///     migrate(*db, listing.migrations, options);
///
/// Every `.sql` file in the directory is then named, in `migrations` or in
/// `known_ids`, and a database that has recorded an id none of them has is
/// refused as `MigrationRefusal::missing`. So the superseded files have to
/// stay where they are: see "A superseded file stays on disk" above.
///
/// Nothing is sent to a database.
///
/// \throws as `migrations_from_directory()`.
[[nodiscard]] DirectoryListing list_migration_directory(const std::filesystem::path& directory,
                                                        Dialect                      dialect,
                                                        const DirectoryOptions& options = {});

} // namespace schemata
