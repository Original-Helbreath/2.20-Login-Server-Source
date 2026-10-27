#pragma once

// MariaDB backend.
//
// Include this header and link sch_mariadb, which links MariaDB
// Connector/C. For Oracle's MySQL client library, use <schemata/mysql.h>
// and sch_mysql instead.
//
// The two drivers are built from the same source: the client libraries
// export the same mysql_* API, so only the linked library and a handful of
// server dialect differences separate them. The same fact means the two
// archives cannot share a binary: link one, and it reaches either server.

#include <memory>

#include <schemata/driver/driver.h>
#include <schemata/mysql.h>

namespace schemata {

/// MariaDB shares MySQL's connection parameters.
using MariaDbOptions = MySqlOptions;

[[nodiscard]] std::unique_ptr<driver::Connection> mariadb(MariaDbOptions options);

} // namespace schemata
