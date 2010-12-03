/*-------------------------------------------------------------------------
 *
 * nodeSort.c
 *	  Routines to handle sorting of relations.
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeSort.c,v 1.67 2010/01/02 16:57:45 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/execdebug.h"
#include "executor/nodeSort.h"
#include "miscadmin.h"
#include "utils/tuplesort.h"
#include "utils/memutils.h"
#include "storage/lock.h"
#include "storage/proc.h"
#include "storage/pmsignal.h"
#include "storage/parallel.h"
#include "postmaster/slaveworker.h"

#include <pthread.h>

/* ----------------------------------------------------------------
 *		ExecSort
 *
 *		Sorts tuples from the outer subtree of the node using tuplesort,
 *		which saves the results in a temporary file or memory. After the
 *		initial call, returns a tuple from the file with each call.
 *
 *		Conditions:
 *		  -- none.
 *
 *		Initial States:
 *		  -- the outer child is prepared to return the first tuple.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecSort(SortState *node)
{
	EState	   *estate;
	ScanDirection dir;
	Tuplesortstate *tuplesortstate;
	TupleTableSlot *slot;
	MemoryContext oldContext;
	WorkDef * work;
	Worker * worker;
	//guc variable
	int prl_level = prl_sort_dop;
	bool prl_on = prl_sort && !isSlaveWorker();
	int i,j, ii;
	SortParams * sortParams;
	MemoryContext currctx;
	long int jobId = 0;	
	
	currctx = CurrentMemoryContext;

	/*
	 * get state info from node
	 */
	SO1_printf("ExecSort: %s\n",
			   "entering routine");

	estate = node->ss.ps.state;
	dir = estate->es_direction;
	tuplesortstate = (Tuplesortstate *) node->tuplesortstate;
	
	// hack for testing
	if (prl_test && !isSlaveWorker()) {
		long int workersId = random();
		TestParams * testParams;
		
		if (ShmMessageContext == NULL) {
			ShmMessageContext = ShmContextCreate (ShmParallelContext,"ShmMessageContext",0,0,0);
		}
		
		oldContext = MemoryContextSwitchTo(ShmMessageContext);
		if (workersList == NULL) {
			workersList = createShList();
		}
		
		for (i=0; i < prl_test_workers; i++) {
			work = (WorkDef*)palloc(sizeof(WorkDef));
			work->new = true;
			work->hasWork = true;
			work->workType = PRL_WORK_TYPE_TEST;
			work->workParams = (WorkParams*)palloc(sizeof(WorkParams));
			work->jobId = workersId;
			testParams = (TestParams *) palloc(sizeof(TestParams));
			testParams->type = prl_test_type;
			testParams->chunk_cnt = prl_test_chunk_cnt;
			testParams->chunk_size = prl_test_chunk_size;
			testParams->cycles = prl_test_cycles;
			work->workParams->testParams = testParams;
			work->workParams->databaseId = MyProc->databaseId;
			work->workParams->roleId = MyProc->roleId;
			work->workParams->username = GetUserNameFromId(MyProc->roleId);
			worker = (Worker*)palloc(sizeof(Worker));
			SpinLockInit(&worker->mutex);
			worker->valid = false;
			worker->state = PRL_WORKER_STATE_NONE;
			worker->work = work;
			work->worker = worker;
			shListAppend(workersList, worker);
			MemoryContextSwitchTo(oldContext);

			oldContext = MemoryContextSwitchTo(ShmParallelContext);
			shListAppend(prlJobsList, work);
			MemoryContextSwitchTo(oldContext);

			oldContext = MemoryContextSwitchTo(ShmMessageContext);

		}
		
		ereport(DEBUG_PRL1,(errmsg("Master: Signalizing Postmaster")));
		SendPostmasterSignal(PMSIGNAL_START_PARALLEL_WORKERS);
		
		// wait until they are ready
		waitForWorkers(workersId, prl_test_workers, PRL_WORKER_STATE_INITIAL);
		MemoryContextSwitchTo(oldContext);
		
		// let them know to start working
		stateTransition(workersId, PRL_WORKER_STATE_INITIAL, PRL_WORKER_STATE_READY);	
		
		waitForWorkers(workersId, prl_test_workers, PRL_WORKER_STATE_END);
		
		slot = node->ss.ps.ps_ResultTupleSlot;
		ExecClearTuple(slot);
		return slot;
	}

	/*
	 * If first time through, read all tuples from outer plan and pass them to
	 * tuplesort.c. Subsequent calls just fetch tuples from tuplesort.
	 */

	if (!node->sort_Done)
	{
		Sort	   *plannode = (Sort *) node->ss.ps.plan;
		PlanState  *outerNode;
		TupleDesc	tupDesc;
		// this will identify workers used in this one sort task
		long int workersId = random();
		ereport(DEBUG_PRL1,(errmsg("Master: Sort - jobId = %ld", workersId)));

		SO1_printf("ExecSort: %s\n",
				   "sorting subplan");

		/*
		 * Want to scan subplan in the forward direction while creating the
		 * sorted data.
		 */
		estate->es_direction = ForwardScanDirection;

		/*
		 * Initialize tuplesort module.
		 */
		SO1_printf("ExecSort: %s\n",
				   "calling tuplesort_begin");

		outerNode = outerPlanState(node);
		tupDesc = ExecGetResultType(outerNode);

		tuplesortstate = tuplesort_begin_heap(tupDesc,
											  plannode->numCols,
											  plannode->sortColIdx,
											  plannode->sortOperators,
											  plannode->nullsFirst,
											  work_mem,
											  node->randomAccess);
		
		if (node->bounded)
			tuplesort_set_bound(tuplesortstate, node->bound);
		node->tuplesortstate = (void *) tuplesortstate;

		tuplesort_set_parallel(tuplesortstate, prl_on);
		
		if (!prl_on) {
			ereport(DEBUG_PRL1,(errmsg("nodeSort std - loading tuples")));
			ii = 0;
			for (;;)
			{
				slot = ExecProcNode(outerNode);
				if (TupIsNull(slot))
					break;
				tuplesort_puttupleslot(tuplesortstate, slot);
			}
			
			
			ereport(DEBUG_PRL1, (errmsg("nodeSort std - before performsort")));
			/*
			 * Complete the sort.
			 */
			tuplesort_performsort(tuplesortstate);
			ereport(DEBUG_PRL1, (errmsg("nodeSort std - after performsort")));
			/*
			 * restore to user specified direction
			 */
			estate->es_direction = dir;
			
			/*
			 * finally set the sorted flag to true
			 */
			node->sort_Done = true;
			node->bounded_Done = node->bounded;
			node->bound_Done = node->bound;
			SO1_printf("ExecSort: %s\n", "sorting done");
		} else {
			tuplesort_set_prl_level(tuplesortstate, prl_level);
			tuplesort_set_workersId(tuplesortstate, workersId);
			node->tuplesortstate = (void *) tuplesortstate;
			
			if (ShmMessageContext == NULL) {
				ShmMessageContext = ShmContextCreate (ShmParallelContext,"ShmMessageContext",0,0,0);
			}
	
			oldContext = MemoryContextSwitchTo(ShmMessageContext);
			
			// kind of on demand but it would be better to do it during init of this backend
			if (workersList == NULL) {
				workersList = createShList();
			}
			
			// prepare work with params and let postmaster create us workers
			for (i=0; i < prl_level; i++) {
				work = (WorkDef*)palloc(sizeof(WorkDef));
				work->new = true;
				work->hasWork = true;
				work->workType = PRL_WORK_TYPE_SORT;
				work->workParams = (WorkParams*)palloc(sizeof(WorkParams));
				work->jobId = workersId;
				sortParams = (SortParams *) palloc(sizeof(SortParams));
				sortParams->bounded = node->bounded;
				sortParams->forward = ScanDirectionIsForward(dir);
				sortParams->numCols = plannode->numCols;
				sortParams->randomAccess = node->randomAccess;
				sortParams->tupDesc = CreateTupleDescCopy(tupDesc);
				if (node->bounded) {
					sortParams->bound = node->bound;
				}
				sortParams->sortColIdx = (AttrNumber *)palloc(sortParams->numCols * sizeof (AttrNumber));
				sortParams->sortOperators = (Oid *)palloc(sortParams->numCols * sizeof (Oid));
				sortParams->nullsFirst = (bool *)palloc(sortParams->numCols * sizeof (bool));
				for (j = 0; j < sortParams->numCols; j++) {
					sortParams->sortColIdx[j] = plannode->sortColIdx[j];
					sortParams->sortOperators[j] = plannode->sortOperators[j];
					sortParams->nullsFirst[j] = plannode->nullsFirst[j];
				}
				work->workParams->sortParams = sortParams;
				work->workParams->databaseId = MyProc->databaseId;
				work->workParams->roleId = MyProc->roleId;
				work->workParams->username = GetUserNameFromId(MyProc->roleId);
				work->workParams->bufferQueue = createBufferQueue(parallel_shared_queue_size);
				
				worker = (Worker*)palloc(sizeof(Worker));
				SpinLockInit(&worker->mutex);
				worker->valid = false;
				worker->state = PRL_WORKER_STATE_NONE;
				worker->work = work;
				work->worker = worker;
				
				shListAppend(workersList, worker);
				MemoryContextSwitchTo(oldContext);

				oldContext = MemoryContextSwitchTo(ShmParallelContext);
				shListAppend(prlJobsList, work);
				MemoryContextSwitchTo(oldContext);

				oldContext = MemoryContextSwitchTo(ShmMessageContext);
			}
			
			ereport(DEBUG_PRL1,(errmsg("Master: Signalizing Postmaster")));
			SendPostmasterSignal(PMSIGNAL_START_PARALLEL_WORKERS);
			
			// wait until they are ready
			waitForWorkers(workersId, prl_level, PRL_WORKER_STATE_INITIAL);
			MemoryContextSwitchTo(oldContext);
			
			registerWorkers(tuplesortstate, prl_level);
			
			// let them know to start working
			stateTransition(workersId, PRL_WORKER_STATE_INITIAL, PRL_WORKER_STATE_READY);
			ereport(DEBUG_PRL1,(errmsg("nodeSort - before sending tuples to workers")));
			/*
			 * Scan the subplan and feed all the tuples to tuplesort.
			 */
			for (;;) {
				slot = ExecProcNode(outerNode);
				oldContext = MemoryContextSwitchTo(ShmParallelContext);
				if (TupIsNull(slot)) {
					distributeToWorker(tuplesortstate, NULL, true);
					MemoryContextSwitchTo(oldContext);
					break;
				}
				distributeToWorker(tuplesortstate, slot, false);
				MemoryContextSwitchTo(oldContext);
			}
			
			ereport(DEBUG_PRL1,(errmsg("nodeSort - waiting until workers finish the job.")));
			// wait until they finish the job
			waitForWorkers(workersId, prl_level, PRL_WORKER_STATE_FINISHED);
	
			ereport(DEBUG_PRL1,(errmsg("nodeSort - workers finished the job.")));
			// get the slaves ready for sending results
			stateTransition(workersId, PRL_WORKER_STATE_FINISHED, PRL_WORKER_STATE_FINISHED_ACK);
			ereport(DEBUG_PRL1,(errmsg("nodeSort - workers set to FINISHED_ACK")));
	
			// fetch also first from each worker
			prepareForMerge(tuplesortstate);
			
			/*
			 * restore to user specified direction
			 */
			estate->es_direction = dir;
	
			/*
			 * finally set the sorted flag to true
			 */
			node->sort_Done = true;
			node->bounded_Done = node->bounded;
			node->bound_Done = node->bound;
			SO1_printf("ExecSort: %s\n", "sorting done");
		}
	}
		
	SO1_printf("ExecSort: %s\n",
			   "retrieving tuple from tuplesort");

	prl_on = tuplesort_is_parallel(tuplesortstate);
	if (InterruptHoldoffCount > 0) {
		ereport(DEBUG_PRL2,(errmsg("Master - Signals blocked.")));
	} else {
		ereport(DEBUG_PRL2,(errmsg("Master - Signals OK.")));
	}
	/*
	 * Get the first or next tuple from tuplesort. Returns NULL if no more
	 * tuples.
	 */
	slot = node->ss.ps.ps_ResultTupleSlot;
	if (prl_on) {
		jobId = tuplesort_get_workersId(tuplesortstate);
		tuplesort_gettupleslot_from_worker(tuplesortstate, ScanDirectionIsForward(dir), slot);
	} else {
		(void) tuplesort_gettupleslot(tuplesortstate,
				ScanDirectionIsForward(dir), slot);
	}
	
	currctx = CurrentMemoryContext;
	
	return slot;
}

/* ----------------------------------------------------------------
 *		ExecInitSort
 *
 *		Creates the run-time state information for the sort node
 *		produced by the planner and initializes its outer subtree.
 * ----------------------------------------------------------------
 */
SortState *
ExecInitSort(Sort *node, EState *estate, int eflags)
{
	SortState  *sortstate;

	SO1_printf("ExecInitSort: %s\n",
			   "initializing sort node");

	/*
	 * create state structure
	 */
	sortstate = makeNode(SortState);
	sortstate->ss.ps.plan = (Plan *) node;
	sortstate->ss.ps.state = estate;

	/*
	 * We must have random access to the sort output to do backward scan or
	 * mark/restore.  We also prefer to materialize the sort output if we
	 * might be called on to rewind and replay it many times.
	 */
	sortstate->randomAccess = (eflags & (EXEC_FLAG_REWIND |
										 EXEC_FLAG_BACKWARD |
										 EXEC_FLAG_MARK)) != 0;

	sortstate->bounded = false;
	sortstate->sort_Done = false;
	sortstate->tuplesortstate = NULL;

	/*
	 * Miscellaneous initialization
	 *
	 * Sort nodes don't initialize their ExprContexts because they never call
	 * ExecQual or ExecProject.
	 */

	/*
	 * tuple table initialization
	 *
	 * sort nodes only return scan tuples from their sorted relation.
	 */
	ExecInitResultTupleSlot(estate, &sortstate->ss.ps);
	ExecInitScanTupleSlot(estate, &sortstate->ss);

	/*
	 * initialize child nodes
	 *
	 * We shield the child node from the need to support REWIND, BACKWARD, or
	 * MARK/RESTORE.
	 */
	eflags &= ~(EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK);

	outerPlanState(sortstate) = ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * initialize tuple type.  no need to initialize projection info because
	 * this node doesn't do projections.
	 */
	ExecAssignResultTypeFromTL(&sortstate->ss.ps);
	ExecAssignScanTypeFromOuterPlan(&sortstate->ss);
	sortstate->ss.ps.ps_ProjInfo = NULL;

	SO1_printf("ExecInitSort: %s\n",
			   "sort node initialized");

	return sortstate;
}

/* ----------------------------------------------------------------
 *		ExecEndSort(node)
 * ----------------------------------------------------------------
 */
void
ExecEndSort(SortState *node)
{
	bool prl_on = false;
	bool lastValue = false;
	int workersCnt = 0;
	Worker * worker;
	BufferQueueCell * bqc;
	ListCell * lc;
	long int jobId = 0;
	SO1_printf("ExecEndSort: %s\n",
			   "shutting down sort node");
	ereport(DEBUG_PRL1,(errmsg("nodeSort - ExecEndSort")));
	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	/* must drop pointer to sort result tuple */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	
	/*
	 * Release tuplesort resources
	 */
	if (node->tuplesortstate != NULL) {
		prl_on = tuplesort_is_parallel(node->tuplesortstate);
		
		if (prl_on) {
			jobId = tuplesort_get_workersId(node->tuplesortstate);
			workersCnt = tuplesort_get_prl_level(node->tuplesortstate);
			ereport(DEBUG_PRL1,(errmsg("nodeSort - try cleaning")));
			HOLD_INTERRUPTS();
			SpinLockAcquire(&workersList->mutex);
			foreach(lc, workersList->list) {
				worker = (Worker *) lfirst(lc);
				SpinLockAcquire(&worker->mutex);
				if (worker->valid && worker->state != PRL_WORKER_STATE_END && worker->work->jobId == jobId) {
					lastValue = bufferQueueSetStop(worker->work->workParams->bufferQueue, true);
					if (!lastValue) {
						ereport(DEBUG_PRL1,(errmsg("nodeSort - clearing one value from queue so worker can notice stop")));
						// clear at least one value, so they have a chance to notice the end
						// sender did not send all tuples ...
						// he might be stuck on semaphore
						// it cant be empty because last one was not put inside
						bqc = bufferQueueGet(worker->work->workParams->bufferQueue, false);
						if (bqc != NULL) {
							if (bqc->last) {
								pfree(bqc);
							} else {
								pfree(((MinimalTuple *)((PrlSortTuple *)bqc->ptr_value)->tuple));
								pfree((PrlSortTuple *)bqc->ptr_value);
								pfree(bqc);
							}
						}
					}
				}
				SpinLockRelease(&worker->mutex);
			}
			SpinLockRelease(&workersList->mutex);
			RESUME_INTERRUPTS();
			
			// wait for them to notice the change
			waitForWorkers(jobId,workersCnt,PRL_WORKER_STATE_END);
			
			// prepare ending task
			HOLD_INTERRUPTS();
			SpinLockAcquire(&workersList->mutex);
			foreach(lc, workersList->list) {
				worker = (Worker *) lfirst(lc);
				SpinLockAcquire(&worker->mutex);
				if (worker->valid && worker->work->jobId == jobId) {
					
					destroyBufferQueue(worker->work->workParams->bufferQueue);
					worker->state = PRL_WORKER_STATE_READY;
					worker->work->workType = PRL_WORK_TYPE_END;
				}
				SpinLockRelease(&worker->mutex);
			}
			SpinLockRelease(&workersList->mutex);
			RESUME_INTERRUPTS();
			
			// wait until they all end
			waitForWorkers(jobId, workersCnt, PRL_WORKER_STATE_DEAD);
			
			// release all
			SpinLockAcquire(&workersList->mutex);
			foreach(lc, workersList->list) {
				worker = (Worker *) lfirst(lc);
				SpinLockAcquire(&worker->mutex);
				if (worker->valid && worker->work->jobId == jobId) {
					ereport(DEBUG_PRL1,(errmsg("nodeSort - performing one bufferqueue cleaning")));
					bqc = bufferQueueGetNoSem(worker->work->workParams->bufferQueue);
					while (bqc != NULL) {
						if (bqc->last) {
							pfree(bqc);
						} else {
							pfree(((MinimalTuple *)((PrlSortTuple *)bqc->ptr_value)->tuple));
							pfree((PrlSortTuple *)bqc->ptr_value);
							pfree(bqc);
						}
						bqc = bufferQueueGetNoSem(worker->work->workParams->bufferQueue);
					}
					// remove from postmaster work list
					shListRemove(prlJobsList, worker->work);
					// remove from my list 
					shListRemoveNoLock(workersList, worker);
				}
				SpinLockRelease(&worker->mutex);
			}
			SpinLockRelease(&workersList->mutex);
			
			ereport(DEBUG_PRL1,(errmsg("nodeSort - finished cleaning")));
		}
		
		tuplesort_end((Tuplesortstate *) node->tuplesortstate);
	}
	node->tuplesortstate = NULL;

	/*
	 * shut down the subplan
	 */
	ExecEndNode(outerPlanState(node));

	SO1_printf("ExecEndSort: %s\n",
			   "sort node shutdown");
}

/* ----------------------------------------------------------------
 *		ExecSortMarkPos
 *
 *		Calls tuplesort to save the current position in the sorted file.
 * ----------------------------------------------------------------
 */
void
ExecSortMarkPos(SortState *node)
{
	/*
	 * if we haven't sorted yet, just return
	 */
	if (!node->sort_Done)
		return;

	tuplesort_markpos((Tuplesortstate *) node->tuplesortstate);
}

/* ----------------------------------------------------------------
 *		ExecSortRestrPos
 *
 *		Calls tuplesort to restore the last saved sort file position.
 * ----------------------------------------------------------------
 */
void
ExecSortRestrPos(SortState *node)
{
	/*
	 * if we haven't sorted yet, just return.
	 */
	if (!node->sort_Done)
		return;

	/*
	 * restore the scan to the previously marked position
	 */
	tuplesort_restorepos((Tuplesortstate *) node->tuplesortstate);
}

void
ExecReScanSort(SortState *node, ExprContext *exprCtxt)
{
	/*
	 * If we haven't sorted yet, just return. If outerplan' chgParam is not
	 * NULL then it will be re-scanned by ExecProcNode, else - no reason to
	 * re-scan it at all.
	 */
	if (!node->sort_Done)
		return;

	/* must drop pointer to sort result tuple */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	/*
	 * If subnode is to be rescanned then we forget previous sort results; we
	 * have to re-read the subplan and re-sort.  Also must re-sort if the
	 * bounded-sort parameters changed or we didn't select randomAccess.
	 *
	 * Otherwise we can just rewind and rescan the sorted output.
	 */
	if (((PlanState *) node)->lefttree->chgParam != NULL ||
		node->bounded != node->bounded_Done ||
		node->bound != node->bound_Done ||
		!node->randomAccess)
	{
		node->sort_Done = false;
		tuplesort_end((Tuplesortstate *) node->tuplesortstate);
		node->tuplesortstate = NULL;

		/*
		 * if chgParam of subnode is not null then plan will be re-scanned by
		 * first ExecProcNode.
		 */
		if (((PlanState *) node)->lefttree->chgParam == NULL)
			ExecReScan(((PlanState *) node)->lefttree, exprCtxt);
	}
	else
		tuplesort_rescan((Tuplesortstate *) node->tuplesortstate);
}
