`pg_get_queryid` – get the last queryid generated/used by pg_stat_statements for a backend pid
=============================================================

`pg_get_queryid` is an extension that provides the last queryid generated/used by `pg_stat_statements` for a given backend pid.

Installation
------------

Clone, compile and install:

    $ git clone https://github.com/pgsentinel/pg_get_queryid.git
    $ cd pg_get_queryid
    $ make
    $ make install

As `pg_get_queryid` uses the `pg_stat_statements` extension (officially bundled with PostgreSQL) to get the queryid, add the following entries to your postgresql.conf file (please follow the order):

    $ shared_preload_libraries = 'pg_stat_statements,pg_get_queryid'

restart the postgresql daemon and create the extension:

    $ psql DB -c "CREATE EXTENSION pg_get_queryid;"

Examples
--------

* Verify that the queryid reported by the extension does exist in `pg_stat_statements`:
```
postgres=# select pid, pg_get_queryid(pid) from pg_stat_activity where query_start is not null; 
  pid  |   pg_get_queryid
-------+---------------------
 30816 |                   0
 30818 | 8039953478936474896
 30819 | 2166313660770913507
 30820 | 3193807212165463071
 30812 |                   0
 30811 |                   0
 30813 |                   0
(7 rows)

postgres=# select count(*) from pg_stat_statements where queryid in (select pg_get_queryid(pid) from pg_stat_activity where query_start is not null);
 count
-------
     3
(1 row)
```
* Join `pg_stat_activity` and `pg_stat_statements` that way:
```
postgres=# select pid,state,pgsa.query,pgss.query from pg_stat_activity pgsa, pg_stat_statements pgss where pg_get_queryid(pid) = pgss.queryid and pid != pg_backend_pid() and query_start is not null;
 pid  | state |                       query                        |                          query
------+-------+----------------------------------------------------+---------------------------------------------------------
 6873 | idle  | select pg_sleep(20) where 1 = 1 and 2 = 2 and 3=3; | select pg_sleep($1) where $2 = $3 and $4 = $5 and $6=$7
(1 row)
```
* with `pg_get_queryid.track_utility` set to true, you can also join for utility statements:
```
postgres=# select pid,state,pgsa.query,pgss.query from pg_stat_activity pgsa, pg_stat_statements pgss where pg_get_queryid(pid) = pgss.queryid and pid != pg_backend_pid() and query_start is not null;
 pid  | state |  query  | query
------+-------+---------+--------
 6954 | idle  | vacuum; | vacuum
(1 row)
```
* you can also join in case of prepared statements:
```
postgres=# select pid,state,pgsa.query,pgss.query from pg_stat_activity pgsa, pg_stat_statements pgss where pg_get_queryid(pid) = pgss.queryid and pid != pg_backend_pid() and query_start is not null;
 pid  | state |    query     |                            query
------+-------+--------------+--------------------------------------------------------------
 6954 | idle  | execute pp1; | prepare pp1 as select pg_sleep($1) where $2 = $3 and $4 = $5
(1 row)
```
Note that the query field being reported in `pg_stat_statements` is the prepare statement itself. It is the expected behavior as documented in `pg_stat_statements.c`:
```
	/*
	 * If it's an EXECUTE statement, we don't track it and don't increment the
	 * nesting level.  This allows the cycles to be charged to the underlying
	 * PREPARE instead (by the Executor hooks), which is much more useful.
	 *
```

* If `pg_stat_statements.track` is set to `all` to also track nested statements (such as statements invoked within functions) then you'll be able to see the top level statement and the nested one that way:

```
postgres=# select pid,pgsa.query,pgss.query from pg_stat_activity pgsa, pg_stat_statements pgss where pg_get_queryid(pid) = pgss.queryid and query_start is not null;
  pid  |      query      |   query
-------+-----------------+-----------
 31190 | call my_proc(); | SELECT $1
(1 row)
```


The extension behavior is controlled by the following GUC:

|         Parameter name              | Data type |                  Description                | Default value | Min value  |
| ----------------------------------- | --------- | ------------------------------------------- | ------------  | -------- |
| pg_get_queryid.track_utility     | boolean      | generate a queryid for utility statements |            true |  |

Remarks
-------------------------

* The extension supports PostgreSQL version 9.6 or higher
* The queryid appears in `pg_stat_statements` once its first execution is finished: it means that you won't be able to find the same queryid (than reported by the extension) in `pg_stat_statements` during the first execution of a statement
* The same is true if the query has already been executed but its entry has been pushed out from `pg_stat_statements` (due to `pg_stat_statements.max` or `pg_stat_statements_reset()`: during the next execution the extension will report the queryid but you won't find it in `pg_stat_statements` until the execution finishes
* A patch is in progress in community PG to get the queryid added to `pg_stat_activity`: [see here](https://commitfest.postgresql.org/32/2069). Once the patch gets committed, the extension would make less sense on newer versions that will get this patch.

Corner cases
-------------------------
* As done in the examples, It's better to filter with `query_start is not null` on `pg_stat_activity` to avoid associating a queryid to a session that is freshly created and did not execute anything yet. This filtering is not needed if the session is active or had already been active 
* The query field in `pg_stat_activity` is updated (through `pgstat_report_activity()`) before the hooks used by `pg_stat_statements` and `pg_get_queryid` get triggered. It means that if your session is doing say thousands of queries per second then you may see differents queries reported in `pg_stat_statements` and `pg_stat_activity` when joining on `pg_get_queryid(pid) = pg_stat_statements.queryid` like:
```
postgres=# select pid,state,pgsa.query,pgss.query from pg_stat_activity pgsa, pg_stat_statements pgss where pg_get_queryid(pid) =
pgss.queryid and pid != pg_backend_pid();\watch 1
 pid  | state  |                                    query                                    |   pg_get_queryid    |
          query                         |       queryid
------+--------+-----------------------------------------------------------------------------+---------------------+--------------
----------------------------------------+---------------------
 9506 | active | UPDATE pgbench_accounts SET abalance = abalance + -1182 WHERE aid = 788451; | 2393149975470207403 | SELECT abalan
ce FROM pgbench_accounts WHERE aid = $1 | 2393149975470207403
(1 row)
```
