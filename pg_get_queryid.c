/*-------------------------------------------------------------------------
 *
 * pg_get_queryid.c
 * 	get the last queryid generated/used by pg_stat_statements for a backend pid
 *
 * This program is open source, licensed under the PostgreSQL license.
 * For license terms, see the LICENSE file.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "miscadmin.h"
#include "access/twophase.h"
#include "parser/analyze.h"
#include "parser/scansup.h"
#include "access/hash.h"
#include "utils/guc.h"
#include "executor/executor.h"

PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(pg_get_queryid);

/* Entry point of library loading */
void _PG_init(void);
void _PG_fini(void);

/* Saved hook values in case of unload */
static shmem_startup_hook_type pgqi_prev_shmem_startup_hook = NULL;
static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;

/* Our hooks */
static void pgqi_shmem_startup(void);
static void pgqi_post_parse_analyze(ParseState *pstate, Query *query);
static void pgqi_ExecutorStart(QueryDesc *queryDesc, int eflags);

static uint64 *QueryIdArray = NULL;
static int get_max_procs_count(void);

#if PG_VERSION_NUM >= 100000
static int get_ql_for_utility(const char *query_text, int query_len, int query_location);
#endif

/* to create queryid in case of utility statements*/
#if PG_VERSION_NUM >= 110000
static uint64 getparsedinfo_hash64_string(const char *str, int len);
#else
static uint32 getparsedinfo_hash32_string(const char *str, int len);
#endif

static bool pgqi_track_utility; /* whether to track utility commands */

/* get max procs */
static int
get_max_procs_count(void)
{
	int count = 0;

	count += MaxBackends;
	count += NUM_AUXILIARY_PROCS;
	count += max_prepared_xacts;

	return count;
}

/*
 * get query length to create queryid for utility statements
 * see pg_stat_statements.c for details
 */
#if PG_VERSION_NUM >= 100000
static int 
get_ql_for_utility(const char *querytext, int query_len, int query_location)
{
	if (query_location >= 0)
	{
		Assert(query_location <= strlen(querytext));
		querytext += query_location;
		/* Length of 0 (or -1) means "rest of string" */
		if (query_len <= 0)
			query_len = strlen(querytext);
		else
			Assert(query_len <= strlen(querytext));
	}
	else
	{
		/* If query location is unknown, distrust query_len as well */
		query_location = 0;
		query_len = strlen(querytext);
	}

	/*
  	 * Discard leading and trailing whitespace, too.  Use scanner_isspace()
  	 * not libc's isspace(), because we want to match the lexer's behavior.
 	 */
	while (query_len > 0 && scanner_isspace(querytext[0]))
		querytext++, query_location++, query_len--;
	while (query_len > 0 && scanner_isspace(querytext[query_len - 1]))
		query_len--;

	return query_len;
}
#endif

/* to create queryid in case of utility statements*/
#if PG_VERSION_NUM >= 110000
static uint64
getparsedinfo_hash64_string(const char *str, int len)
{
	return DatumGetUInt64(hash_any_extended((const unsigned char *) str,
																	len, 0));
}
#else
static uint32
getparsedinfo_hash32_string(const char *str, int len)
{
	return hash_any((const unsigned char *) str, len);
}
#endif

static void
pgqi_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	if (MyProc)
	{
		int i = MyProc - ProcGlobal->allProcs;
		QueryIdArray[i] = queryDesc->plannedstmt->queryId;
	}
}

static void
pgqi_post_parse_analyze(ParseState *pstate, Query *query)
{

	if (prev_post_parse_analyze_hook)
		prev_post_parse_analyze_hook(pstate, query);

	if (MyProc)
	{
		int i = MyProc - ProcGlobal->allProcs;

		#if PG_VERSION_NUM >= 110000
		if (query->queryId != UINT64CONST(0)) {
			QueryIdArray[i] = query->queryId;
		#else
		if (query->queryId != 0) {
			QueryIdArray[i] = query->queryId;
		#endif
		} else if (query->commandType == CMD_UTILITY && pgqi_track_utility) {
				const char *querytext = pstate->p_sourcetext;
				uint64 queryid_utility;
				int query_len = 0;
		#if PG_VERSION_NUM >= 100000
				int query_location = query->stmt_location;
				query_len = query->stmt_len;
				query_len = get_ql_for_utility(querytext, query_len, query_location);
		#else
				query_len = strlen(querytext);
		#endif
		#if PG_VERSION_NUM >= 110000
				queryid_utility = getparsedinfo_hash64_string(querytext, query_len);
				#if PG_VERSION_NUM >= 120000
				/*
				 * If we are unlucky enough to get a hash of zero(invalid), use
				 * queryID as 2 instead, queryID 1 is already in use for normal
				 * statements.
				 */
				if (queryid_utility == UINT64CONST(0))
					queryid_utility = UINT64CONST(2);
				#endif
		#else
				queryid_utility = getparsedinfo_hash32_string(querytext, query_len);
		#endif
				QueryIdArray[i] = queryid_utility;
			}
			else
				QueryIdArray[i] = 0;
	}
}

static void
pgqi_shmem_startup(void)
{
	int size;
	bool   found;
	
	if (pgqi_prev_shmem_startup_hook)
		pgqi_prev_shmem_startup_hook();

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	size = mul_size(sizeof(uint64), get_max_procs_count());
	QueryIdArray = (uint64 *) ShmemInitStruct("pg_get_queryid proc entry array", size, &found);

	if (!found)
		MemSet(QueryIdArray, 0, size);

	LWLockRelease(AddinShmemInitLock);
}

void
_PG_init(void)
{

	if (!process_shared_preload_libraries_in_progress)
		return;

	/*
	 * Define (or redefine) custom GUC variables.
 	 */

	DefineCustomBoolVariable("pg_get_queryid.track_utility",
							"Selects whether utility commands are reported by pg_get_queryid.",
							NULL,
							&pgqi_track_utility,
							true,
							PGC_SUSET,
							0,
							NULL,
							NULL,
							NULL);

	pgqi_prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = pgqi_shmem_startup;
	prev_post_parse_analyze_hook = post_parse_analyze_hook;
	post_parse_analyze_hook = pgqi_post_parse_analyze;
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = pgqi_ExecutorStart;
}

Datum
pg_get_queryid(PG_FUNCTION_ARGS)
{
	int i;

	for (i = 0; i < ProcGlobal->allProcCount; i++)
	{
		PGPROC  *proc = &ProcGlobal->allProcs[i];
		if (proc != NULL && proc->pid != 0 && proc->pid == PG_GETARG_INT32(0))
			return QueryIdArray[i];
	}
	return 0;
}

void
_PG_fini(void)
{
	shmem_startup_hook = pgqi_prev_shmem_startup_hook;
	post_parse_analyze_hook = prev_post_parse_analyze_hook;
	ExecutorStart_hook = prev_ExecutorStart;
}
