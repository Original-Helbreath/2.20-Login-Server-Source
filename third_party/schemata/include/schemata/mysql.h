#pragma once

// MySQL backend.
//
// Include this header and link sch_mysql, which links Oracle's MySQL C
// client library. For MariaDB Connector/C, use <schemata/mariadb.h> and
// sch_mariadb instead -- instead, not as well: the two clients export the
// same functions, so the archives cannot share a binary. Either reaches
// either server; the dialect is read from the server, not the archive.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <schemata/driver/driver.h>

namespace schemata {

/// Whether a connection is encrypted, and how far the server is trusted.
///
/// The names and meanings are MySQL's `--ssl-mode`. Oracle's client takes
/// them as they are. MariaDB Connector/C has no such setting and is given the
/// nearest it does have, which is never looser than asked -- except that it
/// has no "encrypt if offered":
///
/// - `preferred` is each library's own default. For Oracle's client that is
///   to encrypt whenever the server offers it; for Connector/C it is a plain
///   connection unless `ssl_ca` or a client certificate is given.
/// - `disabled` on Connector/C also leaves `ssl_ca` and the certificate
///   unused, since giving it either turns encryption on.
/// - `verify_ca` verifies the host name as well, because Connector/C checks
///   the chain and the name together or not at all.
///
/// So on sch_mariadb, ask for `required` or stricter to be sure of
/// encryption.
enum class SslMode : unsigned char {
    /// Encrypt if the server offers it; the client library's own default.
    preferred,
    /// Never encrypt.
    disabled,
    /// Encrypt or fail, without checking the server's certificate.
    required,
    /// Encrypt, and check the certificate against `ssl_ca`.
    verify_ca,
    /// Encrypt, check the certificate, and check it names the host.
    verify_identity,
};

struct MySqlOptions {
    std::string   host     = "localhost";
    std::uint16_t port     = 3306;
    std::string   user;
    std::string   password;
    std::string   database;

    /// Connection character set.
    ///
    /// utf8mb4 rather than utf8: MySQL's "utf8" is a three-byte subset that
    /// cannot represent anything outside the basic multilingual plane, so
    /// emoji and a good deal of CJK silently fail to round-trip.
    std::string charset = "utf8mb4";

    /// Unix domain socket path, used instead of TCP when non-empty.
    std::string socket;

    /// Have an UPDATE report the rows it matched rather than the rows it
    /// changed: the client's `CLIENT_FOUND_ROWS`.
    ///
    /// Off by default, which is the server's own default. The difference is a
    /// row that already holds the new values: counted here, not counted
    /// without it, and always counted on SQLite. So this is also the setting
    /// under which `affected_rows()` means the same on every engine, and
    /// under which "the UPDATE reached no row" is something a caller can test
    /// for. It applies to every statement on the connection, including an
    /// `INSERT ... ON DUPLICATE KEY UPDATE` that changes nothing, which then
    /// reports 1 rather than 0.
    bool found_rows = false;

    /// Seconds to wait for the connection to be made. Zero is the client
    /// library's default.
    unsigned int connect_timeout_seconds = 0;

    /// Seconds to wait for each read from the server. Zero, the default,
    /// waits as long as the server takes.
    ///
    /// A statement that runs longer fails with a `connection_error`, and the
    /// connection is lost with it. Oracle's client retries a timed-out read
    /// twice, so there a statement can run for three times this long first.
    unsigned int read_timeout_seconds = 0;

    /// Seconds to wait for each write to the server. Zero is the client
    /// library's default.
    unsigned int write_timeout_seconds = 0;

    /// Whether to encrypt, and how far to trust the server. See `SslMode`.
    SslMode ssl_mode = SslMode::preferred;

    /// PEM file of the certificate authorities to trust. Empty means the
    /// client library's default.
    std::string ssl_ca;

    /// PEM files of a client certificate and its key, for a server that asks
    /// for one. Both or neither.
    std::string ssl_cert;
    std::string ssl_key;

    /// Statements to run on every new session, in order.
    ///
    /// For session settings the application relies on, such as
    /// `SET SESSION lock_wait_timeout = 5` or a `time_zone`. Run once the
    /// connection is made, and again whenever the session is reset -- which a
    /// `ConnectionPool` does on every return -- because a reset puts every
    /// session variable back to the server's default. A statement that fails
    /// fails the connection, as a `connection_error` naming it.
    ///
    /// A statement that opens, ends or changes the handling of a transaction
    /// -- `BEGIN`, `COMMIT`, `SET autocommit`, `SET completion_type` -- is
    /// refused: the connection keeps count of its transactions and would stop
    /// being right about them. A `CALL` is not looked inside, and a procedure
    /// that does any of those things is the same mistake.
    ///
    /// A connection whose reset fails here is not one to go on using; the
    /// session is put back to the server's defaults and the error raised, and
    /// a `ConnectionPool` closes it rather than lend it again.
    std::vector<std::string> init_statements;
};

// There is no reconnect option, and none is planned. A client library's
// automatic reconnect -- MYSQL_OPT_RECONNECT, deprecated in MySQL 8.0.34 --
// opens a new session in place of the old one without saying so: the open
// transaction is gone, temporary tables and session variables with it, and
// the statement that noticed is retried against a session nothing was set up
// on. A lost connection is reported instead, as a `connection_error`, and
// replaced by the caller:
//
//     if (!db->is_alive()) {          // a ping: one round trip
//         db = schemata::mysql(options);
//     }
//
// A `ConnectionPool` does exactly this on every checkout, and opening through
// the same options runs `init_statements` on the new session.

[[nodiscard]] std::unique_ptr<driver::Connection> mysql(MySqlOptions options);

} // namespace schemata
