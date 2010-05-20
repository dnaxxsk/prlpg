

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
#include "commands/dbcommands.h"
#include "commands/vacuum.h"
#include "libpq/hba.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
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
#include "utils/syscache.h"

#include "utils/tuplesort.h"

static bool am_slave_worker = false;
static volatile sig_atomic_t got_SIGHUP = false;

static void SigHupHandler(SIGNAL_ARGS);
static void doSort(WorkDef * work, Worker * worker);


bool isSlaveWorker(void) {
	return am_slave_worker;
}

int slaveBackendMain(WorkDef * work) {
	MemoryContext oldContext;
	Worker * worker;
	char	   *dbname;
	sigjmp_buf	local_sigjmp_buf;
	
	// wait one minute so i can attach if i want to ..
	//pg_usleep(60*1000000L);
	ereport(LOG,(errmsg("Worker: Initializing - step 1")));
	set_ps_display("startup-slave", false);
	
	SetProcessingMode(InitProcessing);
	am_slave_worker = true;
	
	ereport(LOG,(errmsg("Worker: Initializing - step 2")));
	
	//TODO - stack_base_ptr?
	
	pqsignal(SIGHUP, SigHupHandler);	/* set flag to read config file */
	pqsignal(SIGINT, StatementCancelHandler);	/* cancel current query */
	pqsignal(SIGTERM, die);		/* cancel current query and exit */
	ereport(LOG,(errmsg("Worker: Initializing - step 3")));
	/*
	 * In a standalone backend, SIGQUIT can be generated from the keyboard
	 * easily, while SIGTERM cannot, so we make both signals do die() rather
	 * than quickdie().
	 */	
	pqsignal(SIGQUIT, quickdie);	/* hard crash time */
	pqsignal(SIGALRM, handle_sig_alarm);		/* timeout conditions */
	ereport(LOG,(errmsg("Worker: Initializing - step 4")));
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
	ereport(LOG,(errmsg("Worker: Initializing - step 5")));
	/*
	 * Reset some signals that are accepted by postmaster but not by backend
	 */
	pqsignal(SIGCHLD, SIG_DFL); /* system() requires this on some platforms */

	pqinitmask();

	sigdelset(&BlockSig, SIGQUIT);

	PG_SETMASK(&BlockSig);		/* block everything except SIGQUIT */
	ereport(LOG,(errmsg("Worker: Initializing - step 6")));
	BaseInit();
	ereport(LOG,(errmsg("Worker: Initializing - step 7")));
	InitProcess();
	ereport(LOG,(errmsg("Worker: Initializing - step 8")));
	// po Inite uz mam pgproc so semaforom kde mozem cakat na pracu ...
	
	//here I should get masters dbname and username
	//ereport(DEBUG3,(errmsg_internal("InitPostgres")));
	InitPostgres(NULL, work->workParams->databaseId, work->workParams->username, &dbname);
	ereport(LOG,(errmsg("Worker: Initializing - step 9")));
	SetProcessingMode(NormalProcessing);
	ereport(LOG,(errmsg("Worker: Initializing - step 10")));
	/*
	 * process any libraries that should be preloaded at backend start (this
	 * likewise can't be done until GUC settings are complete)
	 */
	process_local_preload_libraries();
	ereport(LOG,(errmsg("Worker: Initializing - step 11")));
	// here will be mail loop for slave backend work
	
	oldContext = MemoryContextSwitchTo(ShmParalellContext);
	worker = (Worker*)palloc(sizeof(Worker));
	SpinLockInit(&worker->mutex);
	worker->workerPid = MyProcPid;
	worker->sem = MyProc->sem;
	worker->valid = true;
	worker->state = PRL_WORKER_STATE_INITIAL;
	worker->work = work;
	
	ereport(LOG,(errmsg("Worker: Initialized")));
	
	shListAppend(work->workParams->workersList, worker);
	//SpinLockRelease(&worker->mutex);
	
	// switch right here so we can init work in our local memory
	MemoryContextSwitchTo(oldContext);
	
	// here process errors and cancel query of master too
	if (sigsetjmp(local_sigjmp_buf, 1) != 0) {
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
	
	while (true) {
		HOLD_INTERRUPTS();
		SpinLockAcquire(&worker->mutex);
		if (worker->state == PRL_WORKER_STATE_READY) {
			SpinLockRelease(&worker->mutex);
			RESUME_INTERRUPTS();
			
			if (work->workType == PRL_WORK_TYPE_SORT) {
				ereport(LOG,(errmsg("Worker: Job = SORT")));
				StartTransactionCommand();
				ereport(LOG,(errmsg("Worker: Job = SORT - transaction started")));
				doSort(work, worker);
				ereport(LOG,(errmsg("Worker: Job = SORT - work done")));
				CommitTransactionCommand();
				ereport(LOG,(errmsg("Worker: Job = SORT - transaction commited")));
			} 
			
			break;
		} else {
			SpinLockRelease(&worker->mutex);
			RESUME_INTERRUPTS();
		}
		
		pg_usleep(100000L);
	}
	
	ereport(LOG,(errmsg("Worker: wait till the end")));
	while (true) {
		HOLD_INTERRUPTS();
		SpinLockAcquire(&worker->mutex);
		if (worker->state == PRL_WORKER_STATE_END) {
			worker->state = PRL_WORKER_STATE_END_ACK;
			SpinLockRelease(&worker->mutex);
			RESUME_INTERRUPTS();
			break;
		} else {
			SpinLockRelease(&worker->mutex);
			RESUME_INTERRUPTS();
		}
		pg_usleep(100000L);
	}
	ereport(LOG,(errmsg("Worker: THE END")));
	
	// remove me from masters list 
	//shListRemove(worker->work->workParams->workersList, worker);
	//pfree(worker);
	//shListRemove(prlJobsList, work);
	return 0;
}

static void doSort(WorkDef * work, Worker * worker) {
	MemoryContext oldContext;
	Tuplesortstate *tuplesortstate;
	SortParams * pars = work->workParams->sortParams;
	BufferQueueCell * bqc;
	ereport(LOG,(errmsg("Worker-doSort: start begin_heap")));

	tuplesortstate = tuplesort_begin_heap(pars->tupDesc, pars->numCols,
			pars->sortColIdx, pars->sortOperators, pars->nullsFirst, work_mem,
			pars->randomAccess);
	ereport(LOG,(errmsg("Worker-doSort: before set_bound")));
	if (pars->bounded) {
		tuplesort_set_bound(tuplesortstate, pars->bound);
	}
	
	// here put all from shared buffer queue until last one ...
	ereport(LOG,(errmsg("Worker - doSort - before receiving tuples")));
	while ((bqc = bufferQueueGet(work->workParams->bufferQueue))) {
		if (bqc->last) {
			ereport(LOG,(errmsg("Worker - doSort - received last")));
			pfree(bqc);
			break;
		}
//		ereport(DEBUG1,(errmsg("Worker - doSort - received tuple")));
		//do the logic of tuplesort_puttupleslot(tuplesortstate, slot);
		tuplesort_puttupleslot_prl(tuplesortstate, (PrlSortTuple *)bqc->ptr_value);
		// clear it from shared memory ..
//		ereport(DEBUG1,(errmsg("Worker - doSort - before putted tuple")));
		pfree(((MinimalTuple *)((PrlSortTuple *)bqc->ptr_value)->tuple));
		pfree((PrlSortTuple *)bqc->ptr_value);
		pfree(bqc);
//		ereport(DEBUG1,(errmsg("Worker - doSort - after putted tuple")));
	}
	
	printGetUsage();
	
	ereport(LOG,(errmsg("Worker-doSort: before performsort")));
	tuplesort_performsort(tuplesortstate);
	
	// here send them all back - well better would be to send them as one final run so the master can perform the final merge on its own
	// and reuse these workers for another job 
	ereport(LOG,(errmsg("Worker-doSort: after performsort")));
	// tell the master that we have finished
	HOLD_INTERRUPTS();
	SpinLockAcquire(&worker->mutex);
	worker->state = PRL_WORKER_STATE_FINISHED;
	// TODO - remove dummy
	work->workResult->dummyResult1 = work->workParams->dummyValue1*2;
	SpinLockRelease(&worker->mutex);
	RESUME_INTERRUPTS();
	ereport(LOG,(errmsg("Worker-doSort: set state to FINISHED")));
	// wait for ACK by master (master waits for all slaves to return results)
	while (true) {
		HOLD_INTERRUPTS();
		SpinLockAcquire(&worker->mutex);
		if (worker->state == PRL_WORKER_STATE_FINISHED_ACK) {
			SpinLockRelease(&worker->mutex);
			RESUME_INTERRUPTS();
			break;
		} else {
			SpinLockRelease(&worker->mutex);
			RESUME_INTERRUPTS();
		}
		pg_usleep(100000L);
	}
	
	ereport(LOG,(errmsg("Worker-doSort: now in state FINISHED_ACK, starting to send tuples back to master")));
	oldContext = MemoryContextSwitchTo(ShmParalellContext);
	while (true) {
		BufferQueueCell * bqc = (BufferQueueCell *)palloc(sizeof(BufferQueueCell));
		PrlSortTuple * pstup = (PrlSortTuple *) palloc(sizeof(PrlSortTuple));
		if (prl_tuplesort_getsorttuple(tuplesortstate, pars->forward, pstup)) {
			bqc->last = false;
			bqc->ptr_value = (void *)pstup;
			if (bufferQueueAdd(work->workParams->bufferQueue, bqc, true)) {
				// in LIMIT CLAUSE the receiver will not want more at some point
				ereport(LOG,(errmsg("Worker-doSort - noticed that master does not want more tuples.")));
				break;
			}
		} else {
			bqc->last = true;
			pfree(pstup);
			bufferQueueAdd(work->workParams->bufferQueue, bqc, true);
			// leave the never ending cycle after sending last tuple
			ereport(LOG,(errmsg("Worker-doSort - NOT noticed .. sending last one")));
			break;
		}
	}
	printAddUsage();
	MemoryContextSwitchTo(oldContext);
	
	ereport(LOG,(errmsg("Worker-doSort: end")));
}


static void
SigHupHandler(SIGNAL_ARGS)
{
	got_SIGHUP = true;
}