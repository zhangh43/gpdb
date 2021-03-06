-- start_ignore
SET gp_create_table_random_default_distribution=off;
-- end_ignore
Alter database dsp_db1 set gp_default_storage_options='appendonly=true, blocksize=8192';
Alter role dsp_role1 set gp_default_storage_options='appendonly=true, blocksize=65536';


Select datname,datconfig from pg_database where datname in ('dsp_db1');

select rolname, rolconfig from pg_roles where rolname in ('dsp_role1');
