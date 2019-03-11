CREATE AGGREGATE example_agg(int4) (
    SFUNC = int4larger,
    STYPE = int4
);

ALTER EXTENSION gp_inject_fault ADD AGGREGATE example_agg(int4);
ALTER EXTENSION gp_inject_fault DROP AGGREGATE example_agg(int4);
DROP EXTENSION gp_inject_fault;
-- test create extension with schema
SET search_path TO invalid_path;
CREATE EXTENSION IF NOT EXISTS gp_inject_fault WITH SCHEMA public;
DROP EXTENSION gp_inject_fault;
RESET search_path;
