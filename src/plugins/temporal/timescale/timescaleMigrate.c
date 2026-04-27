//
// FILE            timescaleMigrate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// Schema migration runner. One numbered migration per entry in the
// migrationsV table; each migration is a function that runs idempotent
// SQL and bumps troe_schema_version on success.
//
// Postgres advisory lock serialises across N brokers booting at once.
//

#include <stddef.h>                                       // NULL
#include <stdio.h>                                        // snprintf
#include <stdlib.h>                                       // strtol
#include <libpq-fe.h>                                     // PG*

#include "ktrace/kTrace.h"                                // KT_E, KT_I, KT_V

#include "temporal/timescale/timescaleGlobals.h"          // timescaleConn
#include "temporal/timescale/timescaleMigrate.h"          // Own interface



// -----------------------------------------------------------------------------
//
// Advisory-lock id — arbitrary 64-bit constant. Brokers booting against
// the same database serialise on this lock during migration.
//
#define TIMESCALE_MIGRATE_ADVISORY_LOCK_ID  0x54524F455F4D4742LL  // 'TROE_MGB'

#define MIGRATIONS_MAX  64



// -----------------------------------------------------------------------------
//
// execSimple - run an SQL statement, log + return -1 on error.
//
static int execSimple(const char* sql)
{
  PGresult* res = PQexec(timescaleConn, sql);
  ExecStatusType st = PQresultStatus(res);
  if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK)
  {
    KT_E("timescale: SQL failed: %s — %s", PQerrorMessage(timescaleConn), sql);
    PQclear(res);
    return -1;
  }
  PQclear(res);
  return 0;
}



// -----------------------------------------------------------------------------
//
// Migration #1 — initial schema.
//
// Plain tables (no hypertable conversion). When TimescaleDB is available
// a follow-up migration can convert them. PostGIS is required only for
// the v_geo column, which we defer until needed.
//
static int troeMig001Initial(void)
{
  static const char* sqls[] =
  {
    "CREATE TABLE IF NOT EXISTS troe_entities ("
    "  entity_id   TEXT NOT NULL,"
    "  entity_type TEXT NOT NULL,"
    "  modified_at TIMESTAMPTZ NOT NULL,"
    "  op          TEXT NOT NULL,"
    "  scope       TEXT[],"
    "  PRIMARY KEY (entity_id, modified_at, op)"
    ")",

    "CREATE INDEX IF NOT EXISTS troe_entities_id_modified  ON troe_entities (entity_id, modified_at DESC)",
    "CREATE INDEX IF NOT EXISTS troe_entities_type_modified ON troe_entities (entity_type, modified_at DESC)",

    "CREATE TABLE IF NOT EXISTS troe_attrs ("
    "  entity_id   TEXT NOT NULL,"
    "  entity_type TEXT NOT NULL,"
    "  attr_name   TEXT NOT NULL,"
    "  attr_kind   SMALLINT NOT NULL DEFAULT 0,"
    "  dataset_id  TEXT NOT NULL DEFAULT '',"
    "  modified_at TIMESTAMPTZ NOT NULL,"
    "  observed_at TIMESTAMPTZ,"
    "  op          TEXT NOT NULL,"
    "  v_text      TEXT,"
    "  v_number    DOUBLE PRECISION,"
    "  v_bool      BOOLEAN,"
    "  v_datetime  TIMESTAMPTZ,"
    "  v_compound  JSONB,"
    "  sub_attrs   JSONB,"
    "  PRIMARY KEY (entity_id, attr_name, dataset_id, modified_at, op)"
    ")",

    "CREATE INDEX IF NOT EXISTS troe_attrs_id_attr_observed   ON troe_attrs (entity_id, attr_name, observed_at DESC)",
    "CREATE INDEX IF NOT EXISTS troe_attrs_id_attr_modified   ON troe_attrs (entity_id, attr_name, modified_at DESC)",
    "CREATE INDEX IF NOT EXISTS troe_attrs_type_attr_observed ON troe_attrs (entity_type, attr_name, observed_at DESC)",

    NULL
  };

  for (int i = 0; sqls[i] != NULL; i++)
    if (execSimple(sqls[i]) != 0)
      return -1;

  return 0;
}



// -----------------------------------------------------------------------------
//
// Migration table.
//
typedef struct
{
  int          version;
  const char*  label;
  int       (* sqlFn)(void);
} TroeMigration;

static const TroeMigration migrationsV[] =
{
  { 1, "initial schema", troeMig001Initial },
  { 0, NULL,             NULL              }
};



// -----------------------------------------------------------------------------
//
// currentSchemaVersion - read max(version) from troe_schema_version.
// Returns 0 if the meta-table doesn't exist yet (fresh install).
//
static int currentSchemaVersion(void)
{
  PGresult* res = PQexec(timescaleConn,
                         "SELECT COALESCE(max(version), 0) FROM troe_schema_version");
  ExecStatusType st = PQresultStatus(res);

  if (st != PGRES_TUPLES_OK)
  {
    PQclear(res);
    return 0;  // table doesn't exist; bootstrapper will create it
  }

  int v = (int) strtol(PQgetvalue(res, 0, 0), NULL, 10);
  PQclear(res);
  return v;
}



// -----------------------------------------------------------------------------
//
// timescaleMigrate -
//
int timescaleMigrate(void)
{
  // 1. Acquire advisory lock — serialises N brokers booting at once.
  char lockSql[128];
  snprintf(lockSql, sizeof(lockSql), "SELECT pg_advisory_lock(%lld)",
           (long long) TIMESCALE_MIGRATE_ADVISORY_LOCK_ID);
  if (execSimple(lockSql) != 0)
    return -1;

  // 2. Bootstrap the meta-table if missing.
  if (execSimple(
        "CREATE TABLE IF NOT EXISTS troe_schema_version ("
        "  version    INT PRIMARY KEY,"
        "  applied_at TIMESTAMPTZ NOT NULL DEFAULT now()"
        ")") != 0)
  {
    return -1;
  }

  int current = currentSchemaVersion();
  KT_V("timescale: current schema version = %d", current);

  // 3. Apply pending migrations.
  for (int i = 0; migrationsV[i].sqlFn != NULL; i++)
  {
    if (migrationsV[i].version <= current)
      continue;

    KT_I("timescale: applying migration %d (%s)", migrationsV[i].version, migrationsV[i].label);

    if (execSimple("BEGIN") != 0)                               return -1;
    if (migrationsV[i].sqlFn() != 0)
    {
      execSimple("ROLLBACK");
      return -1;
    }

    char insertSql[128];
    snprintf(insertSql, sizeof(insertSql),
             "INSERT INTO troe_schema_version (version) VALUES (%d) ON CONFLICT DO NOTHING",
             migrationsV[i].version);
    if (execSimple(insertSql) != 0)
    {
      execSimple("ROLLBACK");
      return -1;
    }
    if (execSimple("COMMIT") != 0)                              return -1;
  }

  // 4. Release advisory lock.
  char unlockSql[128];
  snprintf(unlockSql, sizeof(unlockSql), "SELECT pg_advisory_unlock(%lld)",
           (long long) TIMESCALE_MIGRATE_ADVISORY_LOCK_ID);
  execSimple(unlockSql);  // best-effort

  return 0;
}
