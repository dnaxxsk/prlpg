/*
 * PostgreSQL System Views
 *
 * Copyright (c) 1996-2010, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/backend/catalog/system_views.sql,v 1.65 2010/01/02 16:57:36 momjian Exp $
 */

CREATE VIEW pg_roles AS 
    SELECT 
        rolname,
        rolsuper,
        rolinherit,
        rolcreaterole,
        rolcreatedb,
        rolcatupdate,
        rolcanlogin,
        rolconnlimit,
        '********'::text as rolpassword,
        rolvaliduntil,
        setconfig as rolconfig,
        pg_authid.oid
    FROM pg_authid LEFT JOIN pg_db_role_setting s
    ON (pg_authid.oid = setrole AND setdatabase = 0);

CREATE VIEW pg_shadow AS
    SELECT
        rolname AS usename,
        pg_authid.oid AS usesysid,
        rolcreatedb AS usecreatedb,
        rolsuper AS usesuper,
        rolcatupdate AS usecatupd,
        rolpassword AS passwd,
        rolvaliduntil::abstime AS valuntil,
        setconfig AS useconfig
    FROM pg_authid LEFT JOIN pg_db_role_setting s
    ON (pg_authid.oid = setrole AND setdatabase = 0)
    WHERE rolcanlogin;

REVOKE ALL on pg_shadow FROM public;

CREATE VIEW pg_group AS
    SELECT
        rolname AS groname,
        oid AS grosysid,
        ARRAY(SELECT member FROM pg_auth_members WHERE roleid = oid) AS grolist
    FROM pg_authid
    WHERE NOT rolcanlogin;

CREATE VIEW pg_user AS 
    SELECT 
        usename, 
        usesysid, 
        usecreatedb, 
        usesuper, 
        usecatupd, 
        '********'::text as passwd, 
        valuntil, 
        useconfig 
    FROM pg_shadow;

CREATE VIEW pg_rules AS 
    SELECT 
        N.nspname AS schemaname, 
        C.relname AS tablename, 
        R.rulename AS rulename, 
        pg_get_ruledef(R.oid) AS definition 
    FROM (pg_rewrite R JOIN pg_class C ON (C.oid = R.ev_class)) 
        LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace) 
    WHERE R.rulename != '_RETURN';

CREATE VIEW pg_views AS 
    SELECT 
        N.nspname AS schemaname, 
        C.relname AS viewname, 
        pg_get_userbyid(C.relowner) AS viewowner, 
        pg_get_viewdef(C.oid) AS definition 
    FROM pg_class C LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace) 
    WHERE C.relkind = 'v';

CREATE VIEW pg_tables AS 
    SELECT 
        N.nspname AS schemaname, 
        C.relname AS tablename, 
        pg_get_userbyid(C.relowner) AS tableowner, 
        T.spcname AS tablespace,
        C.relhasindex AS hasindexes, 
        C.relhasrules AS hasrules, 
        C.relhastriggers AS hastriggers 
    FROM pg_class C LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace) 
         LEFT JOIN pg_tablespace T ON (T.oid = C.reltablespace)
    WHERE C.relkind = 'r';

CREATE VIEW pg_indexes AS 
    SELECT 
        N.nspname AS schemaname, 
        C.relname AS tablename, 
        I.relname AS indexname, 
        T.spcname AS tablespace,
        pg_get_indexdef(I.oid) AS indexdef 
    FROM pg_index X JOIN pg_class C ON (C.oid = X.indrelid) 
         JOIN pg_class I ON (I.oid = X.indexrelid) 
         LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace) 
         LEFT JOIN pg_tablespace T ON (T.oid = I.reltablespace)
    WHERE C.relkind = 'r' AND I.relkind = 'i';

CREATE VIEW pg_stats AS 
    SELECT 
        nspname AS schemaname, 
        relname AS tablename, 
        attname AS attname, 
        stainherit AS inherited, 
        stanullfrac AS null_frac, 
        stawidth AS avg_width, 
        stadistinct AS n_distinct, 
        CASE
            WHEN stakind1 IN (1, 4) THEN stavalues1
            WHEN stakind2 IN (1, 4) THEN stavalues2
            WHEN stakind3 IN (1, 4) THEN stavalues3
            WHEN stakind4 IN (1, 4) THEN stavalues4
        END AS most_common_vals,
        CASE
            WHEN stakind1 IN (1, 4) THEN stanumbers1
            WHEN stakind2 IN (1, 4) THEN stanumbers2
            WHEN stakind3 IN (1, 4) THEN stanumbers3
            WHEN stakind4 IN (1, 4) THEN stanumbers4
        END AS most_common_freqs,
        CASE
            WHEN stakind1 = 2 THEN stavalues1
            WHEN stakind2 = 2 THEN stavalues2
            WHEN stakind3 = 2 THEN stavalues3
            WHEN stakind4 = 2 THEN stavalues4
        END AS histogram_bounds,
        CASE
            WHEN stakind1 = 3 THEN stanumbers1[1]
            WHEN stakind2 = 3 THEN stanumbers2[1]
            WHEN stakind3 = 3 THEN stanumbers3[1]
            WHEN stakind4 = 3 THEN stanumbers4[1]
        END AS correlation
    FROM pg_statistic s JOIN pg_class c ON (c.oid = s.starelid) 
         JOIN pg_attribute a ON (c.oid = attrelid AND attnum = s.staattnum) 
         LEFT JOIN pg_namespace n ON (n.oid = c.relnamespace) 
    WHERE NOT attisdropped AND has_column_privilege(c.oid, a.attnum, 'select');

REVOKE ALL on pg_statistic FROM public;

CREATE VIEW pg_locks AS 
    SELECT * FROM pg_lock_status() AS L;

CREATE VIEW pg_cursors AS
    SELECT * FROM pg_cursor() AS C;

CREATE VIEW pg_prepared_xacts AS
    SELECT P.transaction, P.gid, P.prepared,
           U.rolname AS owner, D.datname AS database
    FROM pg_prepared_xact() AS P
         LEFT JOIN pg_authid U ON P.ownerid = U.oid
         LEFT JOIN pg_database D ON P.dbid = D.oid;

CREATE VIEW pg_prepared_statements AS
    SELECT * FROM pg_prepared_statement() AS P;

CREATE VIEW pg_settings AS 
    SELECT * FROM pg_show_all_settings() AS A; 

CREATE RULE pg_settings_u AS 
    ON UPDATE TO pg_settings 
    WHERE new.name = old.name DO 
    SELECT set_config(old.name, new.setting, 'f');

CREATE RULE pg_settings_n AS 
    ON UPDATE TO pg_settings 
    DO INSTEAD NOTHING;

GRANT SELECT, UPDATE ON pg_settings TO PUBLIC;

CREATE VIEW pg_timezone_abbrevs AS
    SELECT * FROM pg_timezone_abbrevs();

CREATE VIEW pg_timezone_names AS
    SELECT * FROM pg_timezone_names();

-- Statistics views

CREATE VIEW pg_stat_all_tables AS 
    SELECT 
            C.oid AS relid, 
            N.nspname AS schemaname, 
            C.relname AS relname, 
            pg_stat_get_numscans(C.oid) AS seq_scan, 
            pg_stat_get_tuples_returned(C.oid) AS seq_tup_read, 
            sum(pg_stat_get_numscans(I.indexrelid))::bigint AS idx_scan, 
            sum(pg_stat_get_tuples_fetched(I.indexrelid))::bigint +
            pg_stat_get_tuples_fetched(C.oid) AS idx_tup_fetch, 
            pg_stat_get_tuples_inserted(C.oid) AS n_tup_ins, 
            pg_stat_get_tuples_updated(C.oid) AS n_tup_upd, 
            pg_stat_get_tuples_deleted(C.oid) AS n_tup_del,
            pg_stat_get_tuples_hot_updated(C.oid) AS n_tup_hot_upd,
            pg_stat_get_live_tuples(C.oid) AS n_live_tup, 
            pg_stat_get_dead_tuples(C.oid) AS n_dead_tup,
            pg_stat_get_last_vacuum_time(C.oid) as last_vacuum,
            pg_stat_get_last_autovacuum_time(C.oid) as last_autovacuum,
            pg_stat_get_last_analyze_time(C.oid) as last_analyze,
            pg_stat_get_last_autoanalyze_time(C.oid) as last_autoanalyze
    FROM pg_class C LEFT JOIN 
         pg_index I ON C.oid = I.indrelid 
         LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace) 
    WHERE C.relkind IN ('r', 't')
    GROUP BY C.oid, N.nspname, C.relname;

CREATE VIEW pg_stat_sys_tables AS 
    SELECT * FROM pg_stat_all_tables 
    WHERE schemaname IN ('pg_catalog', 'information_schema') OR
          schemaname ~ '^pg_toast';

CREATE VIEW pg_stat_user_tables AS 
    SELECT * FROM pg_stat_all_tables 
    WHERE schemaname NOT IN ('pg_catalog', 'information_schema') AND
          schemaname !~ '^pg_toast';

CREATE VIEW pg_statio_all_tables AS 
    SELECT 
            C.oid AS relid, 
            N.nspname AS schemaname, 
            C.relname AS relname, 
            pg_stat_get_blocks_fetched(C.oid) - 
                    pg_stat_get_blocks_hit(C.oid) AS heap_blks_read, 
            pg_stat_get_blocks_hit(C.oid) AS heap_blks_hit, 
            sum(pg_stat_get_blocks_fetched(I.indexrelid) - 
                    pg_stat_get_blocks_hit(I.indexrelid))::bigint AS idx_blks_read, 
            sum(pg_stat_get_blocks_hit(I.indexrelid))::bigint AS idx_blks_hit, 
            pg_stat_get_blocks_fetched(T.oid) - 
                    pg_stat_get_blocks_hit(T.oid) AS toast_blks_read, 
            pg_stat_get_blocks_hit(T.oid) AS toast_blks_hit, 
            pg_stat_get_blocks_fetched(X.oid) - 
                    pg_stat_get_blocks_hit(X.oid) AS tidx_blks_read, 
            pg_stat_get_blocks_hit(X.oid) AS tidx_blks_hit 
    FROM pg_class C LEFT JOIN 
            pg_index I ON C.oid = I.indrelid LEFT JOIN 
            pg_class T ON C.reltoastrelid = T.oid LEFT JOIN 
            pg_class X ON T.reltoastidxid = X.oid 
            LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace) 
    WHERE C.relkind IN ('r', 't')
    GROUP BY C.oid, N.nspname, C.relname, T.oid, X.oid;

CREATE VIEW pg_statio_sys_tables AS 
    SELECT * FROM pg_statio_all_tables 
    WHERE schemaname IN ('pg_catalog', 'information_schema') OR
          schemaname ~ '^pg_toast';

CREATE VIEW pg_statio_user_tables AS 
    SELECT * FROM pg_statio_all_tables 
    WHERE schemaname NOT IN ('pg_catalog', 'information_schema') AND
          schemaname !~ '^pg_toast';

CREATE VIEW pg_stat_all_indexes AS 
    SELECT 
            C.oid AS relid, 
            I.oid AS indexrelid, 
            N.nspname AS schemaname, 
            C.relname AS relname, 
            I.relname AS indexrelname, 
            pg_stat_get_numscans(I.oid) AS idx_scan, 
            pg_stat_get_tuples_returned(I.oid) AS idx_tup_read, 
            pg_stat_get_tuples_fetched(I.oid) AS idx_tup_fetch 
    FROM pg_class C JOIN 
            pg_index X ON C.oid = X.indrelid JOIN 
            pg_class I ON I.oid = X.indexrelid 
            LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace) 
    WHERE C.relkind IN ('r', 't');

CREATE VIEW pg_stat_sys_indexes AS 
    SELECT * FROM pg_stat_all_indexes 
    WHERE schemaname IN ('pg_catalog', 'information_schema') OR
          schemaname ~ '^pg_toast';

CREATE VIEW pg_stat_user_indexes AS 
    SELECT * FROM pg_stat_all_indexes 
    WHERE schemaname NOT IN ('pg_catalog', 'information_schema') AND
          schemaname !~ '^pg_toast';

CREATE VIEW pg_statio_all_indexes AS 
    SELECT 
            C.oid AS relid, 
            I.oid AS indexrelid, 
            N.nspname AS schemaname, 
            C.relname AS relname, 
            I.relname AS indexrelname, 
            pg_stat_get_blocks_fetched(I.oid) - 
                    pg_stat_get_blocks_hit(I.oid) AS idx_blks_read, 
            pg_stat_get_blocks_hit(I.oid) AS idx_blks_hit 
    FROM pg_class C JOIN 
            pg_index X ON C.oid = X.indrelid JOIN 
            pg_class I ON I.oid = X.indexrelid 
            LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace) 
    WHERE C.relkind IN ('r', 't');

CREATE VIEW pg_statio_sys_indexes AS 
    SELECT * FROM pg_statio_all_indexes 
    WHERE schemaname IN ('pg_catalog', 'information_schema') OR
          schemaname ~ '^pg_toast';

CREATE VIEW pg_statio_user_indexes AS 
    SELECT * FROM pg_statio_all_indexes 
    WHERE schemaname NOT IN ('pg_catalog', 'information_schema') AND
          schemaname !~ '^pg_toast';

CREATE VIEW pg_statio_all_sequences AS 
    SELECT 
            C.oid AS relid, 
            N.nspname AS schemaname, 
            C.relname AS relname, 
            pg_stat_get_blocks_fetched(C.oid) - 
                    pg_stat_get_blocks_hit(C.oid) AS blks_read, 
            pg_stat_get_blocks_hit(C.oid) AS blks_hit 
    FROM pg_class C 
            LEFT JOIN pg_namespace N ON (N.oid = C.relnamespace) 
    WHERE C.relkind = 'S';

CREATE VIEW pg_statio_sys_sequences AS 
    SELECT * FROM pg_statio_all_sequences 
    WHERE schemaname IN ('pg_catalog', 'information_schema') OR
          schemaname ~ '^pg_toast';

CREATE VIEW pg_statio_user_sequences AS 
    SELECT * FROM pg_statio_all_sequences 
    WHERE schemaname NOT IN ('pg_catalog', 'information_schema') AND
          schemaname !~ '^pg_toast';

CREATE VIEW pg_stat_activity AS 
    SELECT 
            S.datid AS datid,
            D.datname AS datname,
            S.procpid,
            S.usesysid,
            U.rolname AS usename,
            S.application_name,
            S.current_query,
            S.waiting,
            S.xact_start,
            S.query_start,
            S.backend_start,
            S.client_addr,
            S.client_port
    FROM pg_database D, pg_stat_get_activity(NULL) AS S, pg_authid U
    WHERE S.datid = D.oid AND 
            S.usesysid = U.oid;

CREATE VIEW pg_stat_database AS 
    SELECT 
            D.oid AS datid, 
            D.datname AS datname, 
            pg_stat_get_db_numbackends(D.oid) AS numbackends, 
            pg_stat_get_db_xact_commit(D.oid) AS xact_commit, 
            pg_stat_get_db_xact_rollback(D.oid) AS xact_rollback, 
            pg_stat_get_db_blocks_fetched(D.oid) - 
                    pg_stat_get_db_blocks_hit(D.oid) AS blks_read, 
            pg_stat_get_db_blocks_hit(D.oid) AS blks_hit,
            pg_stat_get_db_tuples_returned(D.oid) AS tup_returned,
            pg_stat_get_db_tuples_fetched(D.oid) AS tup_fetched,
            pg_stat_get_db_tuples_inserted(D.oid) AS tup_inserted,
            pg_stat_get_db_tuples_updated(D.oid) AS tup_updated,
            pg_stat_get_db_tuples_deleted(D.oid) AS tup_deleted
    FROM pg_database D;

CREATE VIEW pg_stat_user_functions AS 
    SELECT
            P.oid AS funcid, 
            N.nspname AS schemaname,
            P.proname AS funcname,
            pg_stat_get_function_calls(P.oid) AS calls,
            pg_stat_get_function_time(P.oid) / 1000 AS total_time,
            pg_stat_get_function_self_time(P.oid) / 1000 AS self_time
    FROM pg_proc P LEFT JOIN pg_namespace N ON (N.oid = P.pronamespace)
    WHERE P.prolang != 12  -- fast check to eliminate built-in functions   
          AND pg_stat_get_function_calls(P.oid) IS NOT NULL;

CREATE VIEW pg_stat_bgwriter AS
    SELECT
        pg_stat_get_bgwriter_timed_checkpoints() AS checkpoints_timed,
        pg_stat_get_bgwriter_requested_checkpoints() AS checkpoints_req,
        pg_stat_get_bgwriter_buf_written_checkpoints() AS buffers_checkpoint,
        pg_stat_get_bgwriter_buf_written_clean() AS buffers_clean,
        pg_stat_get_bgwriter_maxwritten_clean() AS maxwritten_clean,
        pg_stat_get_buf_written_backend() AS buffers_backend,
        pg_stat_get_buf_alloc() AS buffers_alloc;

CREATE VIEW pg_user_mappings AS
    SELECT
        U.oid       AS umid,
        S.oid       AS srvid,
        S.srvname   AS srvname,
        U.umuser    AS umuser,
        CASE WHEN U.umuser = 0 THEN
            'public'
        ELSE
            A.rolname
        END AS usename,
        CASE WHEN pg_has_role(S.srvowner, 'USAGE') OR has_server_privilege(S.oid, 'USAGE') THEN
            U.umoptions
        ELSE
            NULL
        END AS umoptions
    FROM pg_user_mapping U
         LEFT JOIN pg_authid A ON (A.oid = U.umuser) JOIN
        pg_foreign_server S ON (U.umserver = S.oid);

REVOKE ALL on pg_user_mapping FROM public;

--
-- We have a few function definitions in here, too.
-- At some point there might be enough to justify breaking them out into
-- a separate "system_functions.sql" file.
--

-- Tsearch debug function.  Defined here because it'd be pretty unwieldy
-- to put it into pg_proc.h

CREATE FUNCTION ts_debug(IN config regconfig, IN document text,
    OUT alias text,
    OUT description text,
    OUT token text,
    OUT dictionaries regdictionary[],
    OUT dictionary regdictionary,
    OUT lexemes text[])
RETURNS SETOF record AS
$$
SELECT 
    tt.alias AS alias,
    tt.description AS description,
    parse.token AS token,
    ARRAY ( SELECT m.mapdict::pg_catalog.regdictionary
            FROM pg_catalog.pg_ts_config_map AS m
            WHERE m.mapcfg = $1 AND m.maptokentype = parse.tokid
            ORDER BY m.mapseqno )
    AS dictionaries,
    ( SELECT mapdict::pg_catalog.regdictionary
      FROM pg_catalog.pg_ts_config_map AS m
      WHERE m.mapcfg = $1 AND m.maptokentype = parse.tokid
      ORDER BY pg_catalog.ts_lexize(mapdict, parse.token) IS NULL, m.mapseqno
      LIMIT 1
    ) AS dictionary,
    ( SELECT pg_catalog.ts_lexize(mapdict, parse.token)
      FROM pg_catalog.pg_ts_config_map AS m
      WHERE m.mapcfg = $1 AND m.maptokentype = parse.tokid
      ORDER BY pg_catalog.ts_lexize(mapdict, parse.token) IS NULL, m.mapseqno
      LIMIT 1
    ) AS lexemes
FROM pg_catalog.ts_parse(
        (SELECT cfgparser FROM pg_catalog.pg_ts_config WHERE oid = $1 ), $2 
    ) AS parse,
     pg_catalog.ts_token_type(
        (SELECT cfgparser FROM pg_catalog.pg_ts_config WHERE oid = $1 )
    ) AS tt
WHERE tt.tokid = parse.tokid
$$
LANGUAGE SQL STRICT STABLE;

COMMENT ON FUNCTION ts_debug(regconfig,text) IS
    'debug function for text search configuration';

CREATE FUNCTION ts_debug(IN document text,
    OUT alias text,
    OUT description text,
    OUT token text,
    OUT dictionaries regdictionary[],
    OUT dictionary regdictionary,
    OUT lexemes text[])
RETURNS SETOF record AS
$$
    SELECT * FROM pg_catalog.ts_debug( pg_catalog.get_current_ts_config(), $1);
$$
LANGUAGE SQL STRICT STABLE;

COMMENT ON FUNCTION ts_debug(text) IS
    'debug function for current text search configuration';

--
-- Redeclare built-in functions that need default values attached to their
-- arguments.  It's impractical to set those up directly in pg_proc.h because
-- of the complexity and platform-dependency of the expression tree
-- representation.  (Note that internal functions still have to have entries
-- in pg_proc.h; we are merely causing their proargnames and proargdefaults
-- to get filled in.)
--

CREATE OR REPLACE FUNCTION
  pg_start_backup(label text, fast boolean DEFAULT false)
  RETURNS text STRICT VOLATILE LANGUAGE internal AS 'pg_start_backup';
