//
// FILE            timescaleMigrate.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
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
static int execSimple(PGconn* conn, const char* sql)
{
  PGresult* res = PQexec(conn, sql);
  ExecStatusType st = PQresultStatus(res);
  if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK)
  {
    KT_E("timescale: SQL failed: %s — %s", PQerrorMessage(conn), sql);
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
// a follow-up step can convert them (see ensureHypertable below).
//
// `troe_attrs.instance_id` is the system-generated NGSI-LD instanceId
// (§ 5.2.5 / § 5.2.6). It's the PK so PATCH/DELETE on
// /attrs/{attr}/{instance} (§ 5.6.14 / § 5.6.15) target a specific row,
// and so Modify can freely bump modified_at without re-keying.
// gen_random_uuid() is built-in since PostgreSQL 13 — no extension needed.
//
static int troeMig001Initial(PGconn* conn)
{
  static const char* sqls[] =
  {
    // Each tenant has its own database (see timescalePool), so there is no
    // tenant column — the connection identifies the tenant.
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

    // troe_attrs: one row per instance. created_at + modified_at are
    // distinct properties of the instance (§ 5.6.14.4) — created_at is
    // set once on insert, modified_at is bumped by Modify-Instance.
    // instance_id stays a globally-unique PK (UUID).
    "CREATE TABLE IF NOT EXISTS troe_attrs ("
    "  instance_id TEXT NOT NULL DEFAULT ('urn:ngsi-ld:Instance:' || gen_random_uuid()),"
    "  entity_id   TEXT NOT NULL,"
    "  entity_type TEXT NOT NULL,"
    "  attr_name   TEXT NOT NULL,"
    "  attr_kind   SMALLINT NOT NULL DEFAULT 0,"
    "  dataset_id  TEXT NOT NULL DEFAULT '',"
    "  created_at  TIMESTAMPTZ NOT NULL,"
    "  modified_at TIMESTAMPTZ NOT NULL,"
    "  observed_at TIMESTAMPTZ,"
    "  op          TEXT NOT NULL,"
    "  v_text      TEXT,"
    "  v_number    DOUBLE PRECISION,"
    "  v_bool      BOOLEAN,"
    "  v_datetime  TIMESTAMPTZ,"
    "  v_compound  JSONB,"
    "  sub_attrs   JSONB,"
    // GeoProperty pushdown (§ 11.3.3): a STORED geography column derived from
    // the GeoJSON geometry kept verbatim in v_compound. The attr_kind guard
    // (3 = LdAttrGeoProperty) keeps ST_GeomFromGeoJSON off non-geo rows so it
    // never errors there; the column is NULL for them. SRID 4326 (WGS84) is
    // the GeoJSON default and matches the current-state 2dsphere index, so
    // ST_DWithin/ST_Within distances are spherical metres — the same units a
    // ?georel near;maxDistance query uses. v_compound stays the source of
    // truth; the write path is untouched (the column is generated).
    "  geo         geography GENERATED ALWAYS AS ("
    "    CASE WHEN attr_kind = 3 AND v_compound IS NOT NULL"
    "         THEN geography(ST_SetSRID(ST_GeomFromGeoJSON(v_compound::text), 4326))"
    "    END) STORED,"
    "  PRIMARY KEY (instance_id)"
    ")",

    "CREATE INDEX IF NOT EXISTS troe_attrs_id_attr_observed   ON troe_attrs (entity_id, attr_name, observed_at DESC)",
    "CREATE INDEX IF NOT EXISTS troe_attrs_id_attr_modified   ON troe_attrs (entity_id, attr_name, modified_at DESC)",
    "CREATE INDEX IF NOT EXISTS troe_attrs_type_attr_observed ON troe_attrs (entity_type, attr_name, observed_at DESC)",
    "CREATE INDEX IF NOT EXISTS troe_attrs_geo                ON troe_attrs USING GIST (geo)",

    NULL
  };

  for (int i = 0; sqls[i] != NULL; i++)
    if (execSimple(conn, sqls[i]) != 0)
      return -1;

  return 0;
}



// -----------------------------------------------------------------------------
//
// troeMig002EntityTypeArray - an Entity may carry more than one type.
//
// § 5.2.6.4.2: an Entity's "type" is one type name or an array of them, and
// § 11.2.3.4 has the Temporal API grow that list ("add them to the list of
// Entity Type names"). A single TEXT column cannot hold it, so both entity_type
// columns become TEXT[]. Existing rows convert to a one-element array, which is
// exactly what they meant.
//
// The two indexes that LED with entity_type are replaced by GIN indexes: the
// type filter is now an array-overlap test, which btree cannot serve.
//
static int troeMig002EntityTypeArray(PGconn* conn)
{
  static const char* sqls[] =
  {
    "ALTER TABLE troe_entities ALTER COLUMN entity_type TYPE TEXT[] USING ARRAY[entity_type]",
    "ALTER TABLE troe_attrs    ALTER COLUMN entity_type TYPE TEXT[] USING ARRAY[entity_type]",

    "DROP INDEX IF EXISTS troe_entities_type_modified",
    "DROP INDEX IF EXISTS troe_attrs_type_attr_observed",

    "CREATE INDEX IF NOT EXISTS troe_entities_type ON troe_entities USING GIN (entity_type)",
    "CREATE INDEX IF NOT EXISTS troe_attrs_type    ON troe_attrs    USING GIN (entity_type)",

    // The attr-name half of the dropped troe_attrs index is still worth having.
    "CREATE INDEX IF NOT EXISTS troe_attrs_attr_observed ON troe_attrs (attr_name, observed_at DESC)",

    NULL
  };

  for (int i = 0; sqls[i] != NULL; i++)
    if (execSimple(conn, sqls[i]) != 0)
      return -1;

  return 0;
}


// -----------------------------------------------------------------------------
//
// troeMig003AttrsPkIncludesTime - let troe_attrs actually become a hypertable.
//
// It never has. TimescaleDB requires the partitioning column in EVERY unique
// index, troe_attrs' primary key was instance_id alone, and so every boot
// against a real TimescaleDB logged
//
//   create_hypertable failed: cannot create a unique index without the column
//   "modified_at" (used in partitioning)
//
// and carried on with a plain table. Silently, in the sense that the message is
// one line at startup and everything works afterwards - just without any of the
// time partitioning, compression or retention that is the entire reason for
// putting temporal history in TimescaleDB rather than in postgres.
//
// The fix is to put modified_at in the key. instance_id stays the LEADING
// column, so the three lookups that use it (WHERE entity_id = $1 AND attr_name =
// $2 AND instance_id = $3) keep their index. Uniqueness weakens from instance_id
// to the pair, which is immaterial: the value is a generated UUID and nothing
// upserts on it - see timescaleEvent.c, "No ON CONFLICT - instance_id (PK) is
// auto-generated by the DEFAULT".
//
// The constraint name is postgres' default for a primary key on this table.
// DROP ... IF EXISTS so a database that has already been through this, or one
// built by a future initial schema, is left alone.
//
static int troeMig003AttrsPkIncludesTime(PGconn* conn)
{
  static const char* sqls[] =
  {
    "ALTER TABLE troe_attrs DROP CONSTRAINT IF EXISTS troe_attrs_pkey",
    "ALTER TABLE troe_attrs ADD  CONSTRAINT troe_attrs_pkey PRIMARY KEY (instance_id, modified_at)",
    NULL
  };

  for (int i = 0; sqls[i] != NULL; i++)
    if (execSimple(conn, sqls[i]) != 0)
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
  int       (* sqlFn)(PGconn* conn);
} TroeMigration;

static const TroeMigration migrationsV[] =
{
  { 1, "initial schema",                troeMig001Initial     },
  { 2, "entity_type becomes a TEXT[]",  troeMig002EntityTypeArray },
  { 3, "troe_attrs PK includes modified_at (hypertable prerequisite)", troeMig003AttrsPkIncludesTime },
  { 0, NULL,                            NULL                  }
};



// -----------------------------------------------------------------------------
//
// currentSchemaVersion - read max(version) from troe_schema_version.
// Returns 0 if the meta-table doesn't exist yet (fresh install).
//
static int currentSchemaVersion(PGconn* conn)
{
  PGresult* res = PQexec(conn,
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
static int ensureHypertable(PGconn* conn)
{
  // 1. Is the TimescaleDB extension installed in this database?
  PGresult* res = PQexec(conn,
                         "SELECT 1 FROM pg_extension WHERE extname = 'timescaledb' LIMIT 1");
  if (PQresultStatus(res) != PGRES_TUPLES_OK)
  {
    KT_E("timescale: pg_extension lookup failed: %s", PQerrorMessage(conn));
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
  res = PQexec(conn,
               "SELECT 1 FROM timescaledb_information.hypertables "
               "WHERE hypertable_name = 'troe_attrs' LIMIT 1");
  if (PQresultStatus(res) != PGRES_TUPLES_OK)
  {
    // Older TimescaleDB versions used _timescaledb_catalog.hypertable —
    // log the lookup failure and skip; conversion can be triggered by
    // an admin manually if needed.
    KT_I("timescale: hypertables view query failed (%s) — skipping auto-conversion",
         PQerrorMessage(conn));
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
  res = PQexec(conn,
               "SELECT create_hypertable('troe_attrs', 'modified_at', "
               "                         chunk_time_interval => INTERVAL '7 days', "
               "                         if_not_exists => TRUE, "
               "                         migrate_data => TRUE)");
  if (PQresultStatus(res) != PGRES_TUPLES_OK)
  {
    KT_E("timescale: create_hypertable failed: %s", PQerrorMessage(conn));
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
int timescaleMigrate(PGconn* conn)
{
  // 1. Acquire advisory lock — serialises N brokers booting at once.
  char lockSql[128];
  snprintf(lockSql, sizeof(lockSql), "SELECT pg_advisory_lock(%lld)",
           (long long) TIMESCALE_MIGRATE_ADVISORY_LOCK_ID);
  if (execSimple(conn, lockSql) != 0)
    return -1;

  // 1b. PostGIS — needed by the troe_attrs.geo generated column (§ 11.3.3
  // geoquery pushdown). Idempotent; runs per tenant database. Logged but
  // not fatal: without PostGIS the CREATE TABLE in migration #1 fails loudly,
  // which is the correct signal that the deployment lacks the extension.
  if (execSimple(conn, "CREATE EXTENSION IF NOT EXISTS postgis") != 0)
    KT_I("timescale: CREATE EXTENSION postgis failed — geoquery pushdown unavailable");

  // 2. Bootstrap the meta-table if missing.
  if (execSimple(conn,
        "CREATE TABLE IF NOT EXISTS troe_schema_version ("
        "  version    INT PRIMARY KEY,"
        "  applied_at TIMESTAMPTZ NOT NULL DEFAULT now()"
        ")") != 0)
  {
    return -1;
  }

  int current = currentSchemaVersion(conn);
  KT_V("timescale: current schema version = %d", current);

  // 3. Apply pending migrations.
  for (int i = 0; migrationsV[i].sqlFn != NULL; i++)
  {
    if (migrationsV[i].version <= current)
      continue;

    KT_I("timescale: applying migration %d (%s)", migrationsV[i].version, migrationsV[i].label);

    if (execSimple(conn, "BEGIN") != 0)                         return -1;
    if (migrationsV[i].sqlFn(conn) != 0)
    {
      execSimple(conn, "ROLLBACK");
      return -1;
    }

    char insertSql[128];
    snprintf(insertSql, sizeof(insertSql),
             "INSERT INTO troe_schema_version (version) VALUES (%d) ON CONFLICT DO NOTHING",
             migrationsV[i].version);
    if (execSimple(conn, insertSql) != 0)
    {
      execSimple(conn, "ROLLBACK");
      return -1;
    }
    if (execSimple(conn, "COMMIT") != 0)                        return -1;
  }

  // 4. Hypertable conversion (idempotent, no-op when TS isn't installed).
  if (ensureHypertable(conn) != 0)
  {
    // Don't fail the boot on hypertable issues — the plain-table path
    // still works. The error is logged inside ensureHypertable().
  }

  // 5. Release advisory lock.
  char unlockSql[128];
  snprintf(unlockSql, sizeof(unlockSql), "SELECT pg_advisory_unlock(%lld)",
           (long long) TIMESCALE_MIGRATE_ADVISORY_LOCK_ID);
  execSimple(conn, unlockSql);  // best-effort

  return 0;
}
