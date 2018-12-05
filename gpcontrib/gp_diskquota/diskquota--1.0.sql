/* contrib/diskquota/diskquota--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION diskquota" to load this file. \quit

CREATE SCHEMA diskquota;

-- Configuration table
CREATE TABLE diskquota.quota_config (targetOid oid, quotatype int, quotalimitMB int8, PRIMARY KEY(targetOid, quotatype));

SELECT pg_catalog.pg_extension_config_dump('diskquota.quota_config', '');

CREATE FUNCTION diskquota.set_schema_quota(text, text)
RETURNS void STRICT
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION diskquota.set_role_quota(text, text)
RETURNS void STRICT
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION diskquota.diskquota_start_worker()
RETURNS void STRICT
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE VIEW diskquota.show_schema_quota_view AS
SELECT pg_namespace.nspname AS schema_name, pg_class.relnamespace AS schema_oid, quota.quotalimitMB AS quota_in_mb, sum(pg_total_relation_size(pg_class.oid)) AS nspsize_in_bytes
FROM pg_namespace, pg_class, diskquota.quota_config AS quota
WHERE pg_class.relnamespace = quota.targetoid AND pg_class.relnamespace = pg_namespace.oid AND quota.quotatype=0
GROUP BY pg_class.relnamespace, pg_namespace.nspname, quota.quotalimitMB;

CREATE VIEW diskquota.show_role_quota_view AS
SELECT pg_roles.rolname AS role_name, pg_class.relowner AS role_oid, quota.quotalimitMB AS quota_in_mb, sum(pg_total_relation_size(pg_class.oid)) AS rolsize_in_bytes
FROM pg_roles, pg_class, diskquota.quota_config AS quota
WHERE pg_class.relowner = quota.targetoid AND pg_class.relowner = pg_roles.oid AND quota.quotatype=1
GROUP BY pg_class.relowner, pg_roles.rolname, quota.quotalimitMB;

CREATE TYPE diskquota.diskquota_active_table_type AS ("TABLE_OID" oid,  "TABLE_SIZE" int8);

CREATE OR REPLACE FUNCTION diskquota.diskquota_fetch_table_stat(int4, oid[]) RETURNS setof diskquota_active_table_type
AS 'MODULE_PATHNAME', 'diskquota_fetch_table_stat'
LANGUAGE C VOLATILE;

SELECT diskquota.diskquota_start_worker();
DROP FUNCTION diskquota.diskquota_start_worker();
