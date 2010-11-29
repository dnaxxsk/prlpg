

#include "postgres.h"

#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_database.h"
#include "commands/async.h"
#include "commands/dbcommands.h"
#include "commands/vacuum.h"
#include "executor/execdebug.h"
#include "libpq/hba.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "nodes/nodes.h"
#include "parser/analyze.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "postmaster/fork_process.h"
#include "postmaster/postmaster.h"
#include "postmaster/slaveworker.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/sinval.h"
#include "tcop/tcopprot.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "utils/tuplesort.h"

static bool am_slave_worker = false;
static volatile sig_atomic_t got_SIGHUP = false;

static void SigHupHandler(SIGNAL_ARGS);
static void doSort(WorkDef * work, Worker * worker);
static void doQuery(WorkDef * work, Worker * worker);
static void doTest(WorkDef * work, Worker * worker);
void WorkerStatementCancelHandler(SIGNAL_ARGS);


bool isSlaveWorker(void) {
	return am_slave_worker;
}

/*
 * Query-cancel signal from postmaster: abort current transaction
 * at soonest convenient time
 */
void
WorkerStatementCancelHandler(SIGNAL_ARGS)
{
	int			save_errno = errno;
	ereport(LOG,(errmsg("Worker: Statement cancel")));

	/*
	 * Don't joggle the elbow of proc_exit
	 */
	if (!proc_exit_inprogress)
	{
		InterruptPending = true;
		QueryCancelPending = true;

		/*
		 * If it's safe to interrupt, and we're waiting for input or a lock,
		 * service the interrupt immediately
		 */
		if (ImmediateInterruptOK && InterruptHoldoffCount == 0 &&
			CritSectionCount == 0)
		{
			/* bump holdoff count to make ProcessInterrupts() a no-op */
			/* until we are done getting ready for it */
			InterruptHoldoffCount++;
			LockWaitCancel();	/* prevent CheckDeadLock from running */
			DisableNotifyInterrupt();
			DisableCatchupInterrupt();
			InterruptHoldoffCount--;
			ProcessInterrupts();
		}
	}

	errno = save_errno;
}


int slaveBackendMain(WorkDef * work) {
	Worker * worker;
//	char	   *dbname;
	sigjmp_buf	local_sigjmp_buf;
	
	// wait one minute so i can attach if i want to ..
	//pg_usleep(60*1000000L);
	ereport(DEBUG_PRL2,(errmsg("Worker: Initializing - step 1")));
	set_ps_display("startup-slave", false);
	
	SetProcessingMode(InitProcessing);
	am_slave_worker = true;
	
	ereport(DEBUG_PRL2,(errmsg("Worker: Initializing - step 2")));
	
	pqsignal(SIGHUP, SigHupHandler);	/* set flag to read config file */
	pqsignal(SIGINT, WorkerStatementCancelHandler);	/* cancel current query */
	pqsignal(SIGTERM, die);		/* cancel current query and exit */
	ereport(DEBUG_PRL2,(errmsg("Worker: Initializing - step 3")));
	/*
	 * In a standalone backend, SIGQUIT can be generated from the keyboard
	 * easily, while SIGTERM cannot, so we make both signals do die() rather
	 * than quickdie().
	 */	
	pqsignal(SIGQUIT, quickdie);	/* hard crash time */
	pqsignal(SIGALRM, handle_sig_alarm);		/* timeout conditions */
	ereport(DEBUG_PRL2,(errmsg("Worker: Initializing - step 4")));
	/*
	 * Ignore failure to write to frontend. Note: if frontend closes
	 * connection, we will notice it and exit cleanly when control next
	 * returns to outer loop.  This seems safer than forcing exit in the midst
	 * of output during who-knows-what operation...
	 */
	pqsignal(SIGPIPE, SIG_IGN);
	// FIXME : zachytavat aj tieto a osetrovat ich 
	//pqsignal(SIGUSR1, CatchupInterruptHandler);
	//pqsignal(SIGUSR2, NotifyInterruptHandler);
	pqsignal(SIGFPE, FloatExceptionHandler);
	ereport(DEBUG_PRL2,(errmsg("Worker: Initializing - step 5")));
	/*
	 * Reset some signals that are accepted by postmaster but not by backend
	 */
	pqsignal(SIGCHLD, SIG_DFL); /* system() requires this on some platforms */

	pqinitmask();

	sigdelset(&BlockSig, SIGQUIT);

	PG_SETMASK(&BlockSig);		/* block everything except SIGQUIT */
	ereport(DEBUG_PRL2,(errmsg("Worker: Initializing - step 6")));
	BaseInit();
	ereport(DEBUG_PRL2,(errmsg("Worker: Initializing - step 7")));
	InitProcess();
	ereport(DEBUG_PRL2,(errmsg("Worker: Initializing - step 8")));
	// po Inite uz mam pgproc so semaforom kde mozem cakat na pracu ...
	// neprijimalo to SIGINT ked ho canceloval master
	PG_SETMASK(&UnBlockSig);
	//here I should get masters dbname and username
	InitPostgres(NULL, work->workParams->databaseId, work->workParams->username, NULL);
	ereport(DEBUG_PRL2,(errmsg("Worker: Initializing - step 9")));
	SetProcessingMode(NormalProcessing);
	ereport(DEBUG_PRL2,(errmsg("Worker: Initializing - step 10")));
	/*
	 * process any libraries that should be preloaded at backend start (this
	 * likewise can't be done until GUC settings are complete)
	 */
	process_local_preload_libraries();
	ereport(DEBUG_PRL2,(errmsg("Worker: Initializing - step 11")));
	
	worker = work->worker;
	SpinLockAcquire(&worker->mutex);
	worker->workerPid = MyProcPid;
	worker->valid = true;
	worker->state = PRL_WORKER_STATE_INITIAL;
	SpinLockRelease(&worker->mutex);

	ereport(LOG,(errmsg("Worker: Initialized")));
	
	//pg_usleep(60 * 1000000L);
	
	//shListAppend(work->workParams->workersList, worker);
	
	MessageContext = AllocSetContextCreate(TopMemoryContext,
										  "MessageContext",
										  ALLOCSET_DEFAULT_MINSIZE,
										  ALLOCSET_DEFAULT_INITSIZE,
										  ALLOCSET_DEFAULT_MAXSIZE);
	
	// here process errors and cancel query of master too
	if (sigsetjmp(local_sigjmp_buf, 1) != 0) {
		ereport(LOG,(errmsg("Worker - after error jump cleanup.")));
		error_context_stack = NULL;

		/* Prevent interrupts while cleaning up */
		HOLD_INTERRUPTS();
		
		SpinLockAcquire(&worker->mutex);
		worker->state = PRL_WORKER_STATE_CANCELED;
		SpinLockRelease(&worker->mutex);

		/*
		 * Forget any pending QueryCancel request, since we're returning to
		 * the idle loop anyway, and cancel the statement timer if running.
		 */
		QueryCancelPending = false;
		disable_sig_alarm(true);
		QueryCancelPending = false; /* again in case timeout occurred */

		/*
		 * Turn off these interrupts too.  This is only needed here and not in
		 * other exception-catching places since these interrupts are only
		 * enabled while we wait for client input.
		 */
		DisableCatchupInterrupt();

		/* Report the error to the client and/or server log */
		EmitErrorReport();

		/*
		 * Make sure debug_query_string gets reset before we possibly clobber
		 * the storage it points at.
		 */
		debug_query_string = NULL;

		/*
		 * Abort the current transaction in order to recover.
		 */
		AbortCurrentTransaction();

		/*
		 * Now return to normal top-level context and clear ErrorContext for
		 * next time.
		 */
		MemoryContextSwitchTo(TopMemoryContext);
		FlushErrorState();

		/*
		 * Dont know if i need this in worker
		 * 
		 * If we were handling an extended-query-protocol message, initiate
		 * skip till next Sync.  This also causes us not to issue
		 * ReadyForQuery (until we get Sync).
		 *
		if (doing_extended_query_message)
			ignore_till_sync = true;

		* We don't have a transaction command open anymore *
		xact_started = false;*/

		/* Now we can allow interrupts again */
		RESUME_INTERRUPTS();
		
		// sofar we dont reuse workers
		return 0;
	}
	
	// handle ERROR
	PG_exception_stack = &local_sigjmp_buf;
	
	while (true) {
		waitForState(worker, PRL_WORKER_STATE_READY);
		
		MemoryContextSwitchTo(MessageContext);
		MemoryContextResetAndDeleteChildren(MessageContext);
			
		if (work->workType == PRL_WORK_TYPE_SORT) {
			ereport(DEBUG_PRL2,(errmsg("Worker: Job = SORT")));
			StartTransactionCommand();
			ereport(DEBUG_PRL2,(errmsg("Worker: Job = SORT - transaction started")));
			doSort(work, worker);
			ereport(DEBUG_PRL2,(errmsg("Worker: Job = SORT - work done")));
			CommitTransactionCommand();
			ereport(DEBUG_PRL2,(errmsg("Worker: Job = SORT - transaction commited")));
		} else if (work->workType == PRL_WORK_TYPE_QUERY) {
			ereport(DEBUG_PRL2,(errmsg("Worker: Job = QUERY")));
			doQuery(work, worker);
			ereport(DEBUG_PRL2,(errmsg("Worker: Job = QUERY - work done")));
		} else if (work->workType == PRL_WORK_TYPE_TEST) {
			ereport(DEBUG_PRL2,(errmsg("Worker: Job = TEST")));
			doTest(work, worker);
			ereport(DEBUG_PRL2,(errmsg("Worker: Job = TEST - work done")));
		} else if (work->workType == PRL_WORK_TYPE_END) {
			// special case of job whose purpose is to end
			ereport(DEBUG_PRL2,(errmsg("Worker: Job = END")));
			break;
		}
		ereport(DEBUG_PRL2,(errmsg("Worker: waiting for next job")));
		
		SpinLockAcquire(&worker->mutex);
		worker->state = PRL_WORKER_STATE_END;
		SpinLockRelease(&worker->mutex);
	}
	
	SpinLockAcquire(&worker->mutex);
	worker->state = PRL_WORKER_STATE_DEAD;
	SpinLockRelease(&worker->mutex);
	
	ereport(DEBUG_PRL2,(errmsg("Worker: THE END")));
	
	return 0;
}

static void doSort(WorkDef * work, Worker * worker) {
	MemoryContext oldContext;
	Tuplesortstate *tuplesortstate;
	SortParams * pars = work->workParams->sortParams;
	BufferQueueCell * bqc;
	ereport(DEBUG_PRL2,(errmsg("Worker-doSort: start begin_heap")));

	tuplesortstate = tuplesort_begin_heap(pars->tupDesc, pars->numCols,
			pars->sortColIdx, pars->sortOperators, pars->nullsFirst, work_mem,
			pars->randomAccess);
	ereport(DEBUG_PRL2,(errmsg("Worker-doSort: before set_bound")));
	if (pars->bounded) {
		tuplesort_set_bound(tuplesortstate, pars->bound);
	}
	
	// here put all from shared buffer queue until last one ...
	ereport(DEBUG_PRL2,(errmsg("Worker - doSort - before receiving tuples")));
	while ((bqc = bufferQueueGet(work->workParams->bufferQueue, true))) {
		if (bqc->last) {
			ereport(LOG,(errmsg("Worker - doSort - received last")));
			pfree(bqc);
			break;
		}
		//do the logic of tuplesort_puttupleslot(tuplesortstate, slot);
		tuplesort_puttupleslot_prl(tuplesortstate, (PrlSortTuple *)bqc->ptr_value);
		// clear it from shared memory ..
		pfree(((MinimalTuple *)((PrlSortTuple *)bqc->ptr_value)->tuple));
		pfree((PrlSortTuple *)bqc->ptr_value);
		pfree(bqc);
	}
	
	ereport(DEBUG_PRL2,(errmsg("Worker-doSort: before performsort")));
	tuplesort_performsort(tuplesortstate);
	
	// here send them all back - well better would be to send them as one final run so the master can perform the final merge on its own
	// and reuse these workers for another job 
	ereport(DEBUG_PRL2,(errmsg("Worker-doSort: after performsort")));
	
	//time to signal cancellation ... 
	//pg_usleep(60 * 1000000L);
	
	ereport(DEBUG_PRL2,(errmsg("Worker-doSort: after performsort - after sleep")));
	if (InterruptHoldoffCount > 0) {
		ereport(DEBUG_PRL2,(errmsg("Worker - Signals blocked.")));
	} else {
		ereport(DEBUG_PRL2,(errmsg("Worker - Signals OK.")));
	}
	
	// tell the master that we have finished
	SpinLockAcquire(&worker->mutex);
	worker->state = PRL_WORKER_STATE_FINISHED;
	SpinLockRelease(&worker->mutex);
	ereport(LOG,(errmsg("Worker-doSort: set state to FINISHED")));
	// wait for ACK by master (master waits for all slaves to return results)
	waitForState(worker, PRL_WORKER_STATE_FINISHED_ACK);
	
	ereport(LOG,(errmsg("Worker-doSort: now in state FINISHED_ACK, starting to send tuples back to master")));
	oldContext = MemoryContextSwitchTo(ShmParallelContext);
	while (true) {
		BufferQueueCell * bqc = (BufferQueueCell *)palloc(sizeof(BufferQueueCell));
		PrlSortTuple * pstup = (PrlSortTuple *) palloc(sizeof(PrlSortTuple));
		if (prl_tuplesort_getsorttuple(tuplesortstate, pars->forward, pstup)) {
			bqc->last = false;
			bqc->ptr_value = (void *)pstup;
			if (bufferQueueAdd(work->workParams->bufferQueue, bqc, true)) {
				// in LIMIT CLAUSE the receiver will not want more at some point
				ereport(DEBUG_PRL2,(errmsg("Worker-doSort - noticed that master does not want more tuples.")));
				break;
			}
		} else {
			bqc->last = true;
			pfree(pstup);
			bufferQueueAdd(work->workParams->bufferQueue, bqc, true);
			// leave the never ending cycle after sending last tuple
			ereport(DEBUG_PRL2,(errmsg("Worker-doSort - NOT noticed .. sending last one")));
			break;
		}
	}
	tuplesort_end(tuplesortstate);
	//printAddUsage();
	MemoryContextSwitchTo(oldContext);
	
	ereport(DEBUG_PRL2,(errmsg("Worker-doSort: end")));
}

static void doQuery(WorkDef * work, Worker * worker) {
	MemoryContext oldcontext;
	List	   *parsetree_list;
	ListCell   *parsetree_item;
	QueryParams * pars = work->workParams->queryParams;
	StartTransactionCommand();
	
	if (pars->copyPlan) {
		PlannedStmt * plStmt;
		QueryDesc  *queryDesc;
		ScanDirection direction;
		long count;
		long nprocessed;
		PrlSendState * pss;
		
		// load plan that master sent us
		plStmt = makeNode(PlannedStmt);
		plStmt->commandType = CMD_SELECT;
		plStmt->hasReturning = false;
		plStmt->canSetTag = true;
		plStmt->transientPlan = false;
		plStmt->planTree = copyObject(pars->subnode);
		plStmt->rtable = copyObject(pars->rtable);
		
		PushActiveSnapshot(GetTransactionSnapshot());
		queryDesc = CreateQueryDesc(plStmt, NULL,
				GetActiveSnapshot(), InvalidSnapshot, None_Receiver, 
				NULL, 0);

		ExecutorStart(queryDesc, 0);

		count = FETCH_ALL;
		direction = ForwardScanDirection;

		// append our sending node
		pss = makeNode(PrlSendState);
		pss->bufferQueue = worker->work->workParams->bufferQueue;
		outerPlanState(pss) = queryDesc->planstate;
		queryDesc->planstate = (PlanState*)pss;

		// execute the plan
		ExecutorRun(queryDesc, direction, count);
		nprocessed = queryDesc->estate->es_processed;
		PopActiveSnapshot();

		ExecutorEnd(queryDesc);
		FreeQueryDesc(queryDesc);
	} else {
	
		oldcontext = MemoryContextSwitchTo(MessageContext);
		parsetree_list = pg_parse_query(pars->query_string);
		
		MemoryContextSwitchTo(oldcontext);
		
		// here should be just one 
		foreach(parsetree_item, parsetree_list)
		{
			Node *parsetree = (Node *) lfirst(parsetree_item);
			bool snapshot_set= false;
			List *querytree_list, *plantree_list;
			QueryDesc  *queryDesc;
			ScanDirection direction;
			long count;
			long nprocessed;
			PrlSendState * pss;
			PlannedStmt * plannedStmt;
			PlannedStmt * plStmt;
			
			CHECK_FOR_INTERRUPTS();
			
			if (analyze_requires_snapshot(parsetree)) {
				PushActiveSnapshot(GetTransactionSnapshot());
				snapshot_set = true;
			}
			
			oldcontext = MemoryContextSwitchTo(MessageContext);
	
			querytree_list = pg_analyze_and_rewrite(parsetree, pars->query_string, NULL, 0);
	
			plantree_list = pg_plan_queries(querytree_list, 0, NULL);
			
			if (snapshot_set)
				PopActiveSnapshot();
	
			/* If we got a cancel signal in analysis or planning, quit */
			CHECK_FOR_INTERRUPTS();
			
			plannedStmt = (PlannedStmt *) linitial(plantree_list);
			if (prl_copy_plan) {
				plStmt = makeNode(PlannedStmt);
				plStmt->commandType = CMD_SELECT;
				plStmt->hasReturning = false;
				plStmt->canSetTag = true;
				plStmt->transientPlan = false;
				plStmt->planTree = copyObject(pars->subnode);
				plStmt->rtable = copyObject(pars->rtable);
				plannedStmt->planTree = copyObject(pars->subnode);
				plannedStmt->rtable = copyObject(pars->rtable);
			} else {
				plStmt = plannedStmt;
			}
			
			PushActiveSnapshot(GetTransactionSnapshot());
			queryDesc = CreateQueryDesc(plStmt,
										pars->query_string,
										GetActiveSnapshot(),
										InvalidSnapshot,
										None_Receiver,
										NULL,
										0);
			
			ExecutorStart(queryDesc, 0);
			
			count = FETCH_ALL;
			direction = ForwardScanDirection;
			
			// append our node
			pss = makeNode(PrlSendState);
			pss->bufferQueue = worker->work->workParams->bufferQueue;
			outerPlanState(pss) = queryDesc->planstate;
			queryDesc->planstate = (PlanState*)pss;
			
			ExecutorRun(queryDesc, direction, count);
			nprocessed = queryDesc->estate->es_processed;
			PopActiveSnapshot();
			
			ExecutorEnd(queryDesc);
			FreeQueryDesc(queryDesc);
		}
	}
	
	CommitTransactionCommand();
}

static void
SigHupHandler(SIGNAL_ARGS)
{
	got_SIGHUP = true;
}

static void doTest(WorkDef * work, Worker * worker) {
	TestParams * pars = work->workParams->testParams;
	int i = 0, j = 0;
	long int duration_u = 0;
	long int duration_s = 0;
	
	long int duration_u2 = 0;
	long int duration_s2 = 0;
	struct timeval tv;
	if (pars->type == 1) {
		MemoryContext testContext = AllocSetContextCreate(TopMemoryContext,	"TestContext",	ALLOCSET_DEFAULT_MINSIZE,	ALLOCSET_DEFAULT_INITSIZE,	ALLOCSET_DEFAULT_MAXSIZE);
		MemoryContext oldContext;
		oldContext = MemoryContextSwitchTo(testContext);
		
		gettimeofday(&tv, NULL);
		duration_u = tv.tv_usec;
		duration_s = tv.tv_sec;
		for (i = 0; i < pars->cycles; i++) {
			char ** array;
			gettimeofday(&tv, NULL);
			duration_u2 = tv.tv_usec;
			duration_s2 = tv.tv_sec;
			
			array = palloc0(pars->chunk_cnt * sizeof(char *));
			for (j = 0; j < pars->chunk_cnt; j++) {
				array[j] = palloc0(pars->chunk_size * sizeof(char));
			}

			gettimeofday(&tv, NULL);
			duration_s2 = tv.tv_sec - duration_s2; 
			duration_u2 = duration_s2 * 1000000 + tv.tv_usec - duration_u2;
			ereport(LOG,(errmsg("Worker-doTest alloc n.%d, Cycle=%d, chunks=%d of size=%d took %ld.%06ld seconds", 
							pars->type, i, pars->chunk_cnt, pars->chunk_size, duration_u2 / 1000000, duration_u2 % 1000000)));
			
			gettimeofday(&tv, NULL);
			duration_u2 = tv.tv_usec;
			duration_s2 = tv.tv_sec;
			
			for (j = 0; j < pars->chunk_cnt; j++) {
				pfree(array[j]);
			}
			pfree(array);
			
			gettimeofday(&tv, NULL);
			duration_s2 = tv.tv_sec - duration_s2; 
			duration_u2 = duration_s2 * 1000000 + tv.tv_usec - duration_u2;
			ereport(LOG,(errmsg("Worker-doTest free n.%d, Cycles=%d, chunks=%d of size=%d took %ld.%06ld seconds", 
							pars->type, i, pars->chunk_cnt, pars->chunk_size, duration_u2 / 1000000, duration_u2 % 1000000)));
		}
		gettimeofday(&tv, NULL);
		duration_s = tv.tv_sec - duration_s; 
		duration_u = duration_s * 1000000 + tv.tv_usec - duration_u;
		ereport(LOG,(errmsg("Worker-doTest n.%d, cycles=%d, chunks=%d of size=%d took %ld.%06ld seconds", 
				pars->type, pars->cycles, pars->chunk_cnt, pars->chunk_size, duration_u / 1000000, duration_u % 1000000)));
		MemoryContextSwitchTo(oldContext);
	} else if (pars->type == 2) {
		MemoryContext testContext = ShmParallelContext;
		MemoryContext oldContext;
		oldContext = MemoryContextSwitchTo(testContext);

		gettimeofday(&tv, NULL);
		duration_u = tv.tv_usec;
		duration_s = tv.tv_sec;
		for (i = 0; i < pars->cycles; i++) {
			char ** array;
			gettimeofday(&tv, NULL);
			duration_u2 = tv.tv_usec;
			duration_s2 = tv.tv_sec;

			array = palloc0(pars->chunk_cnt * sizeof(char *));
			for (j = 0; j < pars->chunk_cnt; j++) {
				array[j] = palloc0(pars->chunk_size * sizeof(char));
			}

			gettimeofday(&tv, NULL);
			duration_s2 = tv.tv_sec - duration_s2;
			duration_u2 = duration_s2 * 1000000 + tv.tv_usec - duration_u2;
			ereport(LOG,(errmsg("Worker-doTest alloc n.%d, Cycle=%d, chunks=%d of size=%d took %ld.%06ld seconds",
									pars->type, i, pars->chunk_cnt, pars->chunk_size, duration_u2 / 1000000, duration_u2 % 1000000)));

			gettimeofday(&tv, NULL);
			duration_u2 = tv.tv_usec;
			duration_s2 = tv.tv_sec;

			for (j = 0; j < pars->chunk_cnt; j++) {
				pfree(array[j]);
			}
			pfree(array);

			gettimeofday(&tv, NULL);
			duration_s2 = tv.tv_sec - duration_s2; 
			duration_u2 = duration_s2 * 1000000 + tv.tv_usec - duration_u2;
			ereport(LOG,(errmsg("Worker-doTest free n.%d, Cycles=%d, chunks=%d of size=%d took %ld.%06ld seconds",
									pars->type, i, pars->chunk_cnt, pars->chunk_size, duration_u2 / 1000000, duration_u2 % 1000000)));
		}
		gettimeofday(&tv, NULL);
		duration_s = tv.tv_sec - duration_s; 
		duration_u = duration_s * 1000000 + tv.tv_usec - duration_u;
		ereport(LOG,(errmsg("Worker-doTest n.%d, cycles=%d, chunks=%d of size=%d took %ld.%06ld seconds",
								pars->type, pars->cycles, pars->chunk_cnt, pars->chunk_size, duration_u / 1000000, duration_u % 1000000)));
		MemoryContextSwitchTo(oldContext);
	} else if (pars->type == 3) {
		MemoryContext testContext = ShmParallelContext;
		MemoryContext oldContext;
		oldContext = MemoryContextSwitchTo(testContext);

		gettimeofday(&tv, NULL);
		duration_u = tv.tv_usec;
		duration_s = tv.tv_sec;
		for (i = 0; i < pars->cycles; i++) {
			char ** array;
			gettimeofday(&tv, NULL);
			duration_u2 = tv.tv_usec;
			duration_s2 = tv.tv_sec;

			// trick - realloc is just mm-malloc
			array = ShmParallelContext->methods->realloc(ShmParallelContext, NULL, pars->chunk_cnt * sizeof(char *));
			for (j = 0; j < pars->chunk_cnt; j++) {
				array[j] = ShmParallelContext->methods->realloc(ShmParallelContext, NULL, pars->chunk_size * sizeof(char));
			}

			gettimeofday(&tv, NULL);
			duration_s2 = tv.tv_sec - duration_s2;
			duration_u2 = duration_s2 * 1000000 + tv.tv_usec - duration_u2;
			ereport(LOG,(errmsg("Worker-doTest alloc n.%d, Cycle=%d, chunks=%d of size=%d took %ld.%06ld seconds",
									pars->type, i, pars->chunk_cnt, pars->chunk_size, duration_u2 / 1000000, duration_u2 % 1000000)));

			gettimeofday(&tv, NULL);
			duration_u2 = tv.tv_usec;
			duration_s2 = tv.tv_sec;

			for (j = 0; j < pars->chunk_cnt; j++) {
				//pfree(array[j]);
				ShmParallelContext->methods->realloc(ShmParallelContext, array[j], 0);
			}
			ShmParallelContext->methods->realloc(ShmParallelContext, array, 0);
			//pfree(array);

			gettimeofday(&tv, NULL);
			duration_s2 = tv.tv_sec - duration_s2;
			duration_u2 = duration_s2 * 1000000 + tv.tv_usec - duration_u2;
			ereport(LOG,(errmsg("Worker-doTest free n.%d, Cycles=%d, chunks=%d of size=%d took %ld.%06ld seconds",
									pars->type, i, pars->chunk_cnt, pars->chunk_size, duration_u2 / 1000000, duration_u2 % 1000000)));
		}
		gettimeofday(&tv, NULL);
		duration_s = tv.tv_sec - duration_s;
		duration_u = duration_s * 1000000 + tv.tv_usec - duration_u;
		ereport(LOG,(errmsg("Worker-doTest n.%d, cycles=%d, chunks=%d of size=%d took %ld.%06ld seconds",
								pars->type, pars->cycles, pars->chunk_cnt, pars->chunk_size, duration_u / 1000000, duration_u % 1000000)));
		MemoryContextSwitchTo(oldContext);
	}
	
	return;
}
