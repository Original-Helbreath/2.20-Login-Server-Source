#pragma once

// Schemata -- code-first relational mapping and schema management for C++.
//
// This umbrella header pulls in everything that is backend-agnostic: the
// model, the query DSL, the schema differ and the context. It deliberately
// declares no connection factory, because choosing a backend is a link-time
// decision rather than a preprocessor one.
//
// To connect, include the header for the engine you want and link the
// matching archive:
//
//     <schemata/sqlite.h>   -> sch_sqlite
//     <schemata/mysql.h>    -> sch_mysql
//     <schemata/mariadb.h>  -> sch_mariadb
//
// All of them need sch_core.

#include <schemata/core/converter.h>
#include <schemata/core/error.h>
#include <schemata/core/identifier.h>
#include <schemata/core/types.h>
#include <schemata/core/value.h>
#include <schemata/core/version.h>
#include <schemata/driver/driver.h>
#include <schemata/model/entity.h>
#include <schemata/model/model.h>
#include <schemata/context/context.h>
#include <schemata/context/pool.h>
#include <schemata/query/bulk.h>
#include <schemata/query/join.h>
#include <schemata/query/materialise.h>
#include <schemata/query/plan.h>
#include <schemata/query/query.h>
#include <schemata/query/rows.h>
#include <schemata/query/table_target.h>
#include <schemata/query/tracker.h>
#include <schemata/scaffold/emit.h>
#include <schemata/schema/differ.h>
#include <schemata/schema/manage.h>
#include <schemata/schema/migrate.h>
#include <schemata/schema/migration_files.h>
#include <schemata/schema/snapshot.h>
#include <schemata/sql/ddl.h>
#include <schemata/sql/dialect.h>
#include <schemata/sql/dml.h>
#include <schemata/sql/expr.h>
#include <schemata/sql/script.h>
