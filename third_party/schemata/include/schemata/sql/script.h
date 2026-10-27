#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <schemata/sql/dialect.h>

namespace schemata {

/// Split a script into its statements, the way the engine's own client would.
///
/// For a `.sql` file -- a seed, a dump, a hand-written migration -- that has to
/// reach `migrate()` or `Connection::execute()` one statement at a time. Each
/// statement comes back without its terminator and without the whitespace
/// around it; a comment inside or in front of a statement stays with it, and a
/// stretch holding only comments is dropped.
///
/// A terminator only counts outside a literal, a quoted identifier and a
/// comment, and what those are follows `dialect`:
///
/// - Both: `'...'` with a doubled quote inside, `` `...` ``, `/* ... */`.
/// - MySQL and MariaDB: `"..."` is a string, and a backslash escapes the next
///   character in either kind of string -- the server's default, which
///   `NO_BACKSLASH_ESCAPES` turns off. `#` starts a line comment, and so does
///   `--` followed by a space, a control character or the end, but not
///   `--1`, which is minus minus one. `/*! ... */` is code to the server and
///   kept whole, a terminator inside it included -- more lenient than the
///   `mysql` client, which would split there and is why a dump wraps such a
///   body in `DELIMITER ;;` as well.
///   `DELIMITER $$` on a line of its own changes the terminator until the
///   next one, as the `mysql` client does; that is how a procedure or a
///   trigger with a `BEGIN ... END` body is written in a script, and without
///   it such a body is split at its first semicolon, by the client too.
/// - SQLite: `"..."` and `[...]` are identifiers, a backslash is an ordinary
///   character, and `--` always starts a comment. A `CREATE TRIGGER` runs to
///   the `END` closing its body, semicolons inside it included, with `CASE
///   ... END` inside the body told apart from it. A trigger body the script
///   never closes is refused, since everything after it would be one statement.
///
/// A statement holding a `BEGIN ... END` body -- a trigger or procedure --
/// comes back whole, and `migrate()` takes it as the one statement it is.
///
/// \throws query_build_error when a literal, a quoted identifier, a block
///         comment or a SQLite trigger body is still open at the end of the
///         script, naming the line it opened on -- splitting such a script
///         could only guess.
[[nodiscard]] std::vector<std::string> split_script(std::string_view script, Dialect dialect);

} // namespace schemata
