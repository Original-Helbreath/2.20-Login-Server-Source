#pragma once

#include <compare>
#include <string_view>

namespace schemata {

/// The SQL dialects Schemata emits.
///
/// MySQL and MariaDB share a client API and the bulk of their syntax, but
/// they diverge enough in DDL and feature availability to be tracked
/// separately. The dialect a connection reports is determined at runtime
/// from the server version string, not from which archive you linked.
enum class Dialect {
    sqlite,
    mysql,
    mariadb,
};

[[nodiscard]] constexpr std::string_view to_string(Dialect d) noexcept {
    switch (d) {
        case Dialect::sqlite:  return "sqlite";
        case Dialect::mysql:   return "mysql";
        case Dialect::mariadb: return "mariadb";
    }
    return "unknown";
}

/// True when the dialect uses the MySQL family's syntax and client API.
[[nodiscard]] constexpr bool is_mysql_family(Dialect d) noexcept {
    return d == Dialect::mysql || d == Dialect::mariadb;
}

/// The character that delimits a quoted identifier in this dialect.
///
/// MySQL and MariaDB use a backtick; SQLite uses a double quote. SQLite
/// also accepts backticks and square brackets for compatibility, but emits
/// the standard form.
[[nodiscard]] constexpr char identifier_delimiter(Dialect d) noexcept {
    return is_mysql_family(d) ? '`' : '"';
}

/// A server version, as three numbers.
///
/// `Connection::server_version()` hands back whatever the server felt like
/// saying, and the two families say it differently: `8.4.0`, `5.7.44-log`,
/// `10.11.6-MariaDB`, `12.3.1-MariaDB-log`. Both put the numbers first, so
/// the numbers are what this keeps.
struct ServerVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;

    [[nodiscard]] auto operator<=>(const ServerVersion&) const = default;

    /// False when nothing could be parsed, which is the only honest answer
    /// for a server that reported something unexpected. Gate on a known
    /// version or not at all -- never on a guess.
    [[nodiscard]] bool known() const noexcept { return major != 0; }
};

/// Parse the leading `major[.minor[.patch]]` of a reported version.
///
/// Anything after the numbers is ignored, and a string that does not start
/// with a digit gives an unknown version rather than an error: a server that
/// will not say what it is should not stop anything from running.
[[nodiscard]] ServerVersion parse_server_version(std::string_view reported);

} // namespace schemata
