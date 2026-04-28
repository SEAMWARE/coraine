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
#include <stdbool.h>                                      // bool
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
// Migration #2 — instance_id column (§ 5.2.5 / § 5.2.6 instanceId).
//
// Lets PATCH/DELETE on /attrs/{attr}/{instance} (§ 5.6.14 / § 5.6.15) target
// a specific row. URN-form (urn:ngsi-ld:Instance:<uuid>) so the value never
// contains `/` and stays clean in URL paths.
//
// gen_random_uuid() is built-in since PostgreSQL 13 — no extension needed.
//
static int troeMig002InstanceId(void)
{
  static const char* sqls[] =
  {
    "ALTER TABLE troe_attrs "
    "  ADD COLUMN IF NOT EXISTS instance_id TEXT",

    // Backfill any pre-existing rows.
    "UPDATE troe_attrs "
    "   SET instance_id = 'urn:ngsi-ld:Instance:' || gen_random_uuid() "
    " WHERE instance_id IS NULL",

    "ALTER TABLE troe_attrs "
    "  ALTER COLUMN instance_id SET NOT NULL",

    "ALTER TABLE troe_attrs "
    "  ALTER COLUMN instance_id "
    "  SET DEFAULT 'urn:ngsi-ld:Instance:' || gen_random_uuid()",

    "CREATE UNIQUE INDEX IF NOT EXISTS troe_attrs_instance_id "
    "  ON troe_attrs (instance_id)",

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
  { 1, "initial schema",                 troeMig001Initial    },
  { 2, "instance_id column on troe_attrs", troeMig002InstanceId },
  { 0, NULL,                             NULL                 }
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
// ensureHypertable - convert troe_attrs to a TimescaleDB hypertable when
// the TimescaleDB extension is available.
//
// Runs on every boot, after the versioned migrations. Idempotent:
//   - No TS extension installed → log notice, return 0 (still success).
//   - troe_attrs already a hypertable → no-op.
//   - Otherwise convert with migrate_data => TRUE so any existing rows
//     are partitioned in place.
//
// Done outside the version table on purpose: if a deployment installs
// TimescaleDB later, the conversion happens automatically on the next
// boot without a "rerun pending migrations" dance.
//
static int ensureHypertable(void)
{
  // 1. Is the TimescaleDB extension installed in this database?
  PGresult* res = PQexec(timescaleConn,
                         "SELECT 1 FROM pg_extension WHERE extname = 'timescaledb' LIMIT 1");
  if (PQresultStatus(res) != PGRES_TUPLES_OK)
  {
    KT_E("timescale: pg_extension lookup failed: %s", PQerrorMessage(timescaleConn));
    PQclear(res);
    return -1;
  }
  bool tsInstalled = (PQntuples(res) > 0);
  PQclear(res);

  if (!tsInstalled)
  {
    KT_I("timescale: TimescaleDB extension not installed — running on plain postgres "
         "(troe_attrs will not be a hypertable)");
    return 0;
  }

  // 2. Already a hypertable?
  res = PQexec(timescaleConn,
               "SELECT 1 FROM timescaledb_information.hypertables "
               "WHERE hypertable_name = 'troe_attrs' LIMIT 1");
  if (PQresultStatus(res) != PGRES_TUPLES_OK)
  {
    // Older TimescaleDB versions used _timescaledb_catalog.hypertable —
    // log the lookup failure and skip; conversion can be triggered by
    // an admin manually if needed.
    KT_I("timescale: hypertables view query failed (%s) — skipping auto-conversion",
         PQerrorMessage(timescaleConn));
    PQclear(res);
    return 0;
  }
  bool alreadyHypertable = (PQntuples(res) > 0);
  PQclear(res);

  if (alreadyHypertable)
  {
    KT_V("timescale: troe_attrs is already a hypertable");
    return 0;
  }

  // 3. Convert. modified_at is NOT NULL on every row (broker-receipt time)
  // so it's the safe time-axis. migrate_data => TRUE handles any rows
  // that were already inserted before this conversion.
  KT_I("timescale: converting troe_attrs to hypertable (chunk_time_interval = 7 days)");
  res = PQexec(timescaleConn,
               "SELECT create_hypertable('troe_attrs', 'modified_at', "
               "                         chunk_time_interval => INTERVAL '7 days', "
               "                         if_not_exists => TRUE, "
               "                         migrate_data => TRUE)");
  if (PQresultStatus(res) != PGRES_TUPLES_OK)
  {
    KT_E("timescale: create_hypertable failed: %s", PQerrorMessage(timescaleConn));
    PQclear(res);
    return -1;
  }
  PQclear(res);

  KT_I("timescale: troe_attrs converted to hypertable");
  return 0;
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

  // 4. Hypertable conversion (idempotent, no-op when TS isn't installed).
  if (ensureHypertable() != 0)
  {
    // Don't fail the boot on hypertable issues — the plain-table path
    // still works. The error is logged inside ensureHypertable().
  }

  // 5. Release advisory lock.
  char unlockSql[128];
  snprintf(unlockSql, sizeof(unlockSql), "SELECT pg_advisory_unlock(%lld)",
           (long long) TIMESCALE_MIGRATE_ADVISORY_LOCK_ID);
  execSimple(unlockSql);  // best-effort

  return 0;
}
