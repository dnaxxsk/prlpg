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

#include <pthread.h>

void * threadedPerformSort(void * data)
{
	Tuplesortstate * state;
   state = (Tuplesortstate *) data; 
   tuplesort_performsort(state);
   pthread_exit(NULL);
}

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
	Tuplesortstate * tuplesortstates[2];
	TupleTableSlot *slot;
	MemoryContext oldContext;
	WorkDef * work;
	Worker * worker;
	//guc variable
	int prl_level = parallel_sort_level;
	bool prl_on = parallel_execution_allowed;
	int i,j;
	ListCell * lc;
	int readyCnt = 0;
	SortParams * sortParams;
	MemoryContext currctx;
	long int jobId = 0;
	// DO NOT TURN ON THIS THREADED BEAST !!!!! ;-)
	bool lts = false;
	
	currctx = CurrentMemoryContext;

	/*
	 * get state info from node
	 */
	SO1_printf("ExecSort: %s\n",
			   "entering routine");

	estate = node->ss.ps.state;
	dir = estate->es_direction;
	tuplesortstate = (Tuplesortstate *) node->tuplesortstate;

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
		ereport(LOG,(errmsg("Master: Sort - jobId = %ld", workersId)));		

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

		if (lts) {
			// create two slots for tapesets
			prepare_for_multiLTS(tuplesortstate, 2);
			tuplesortstates[0] = tuplesort_begin_heap(tupDesc,
												  plannode->numCols,
												  plannode->sortColIdx,
												  plannode->sortOperators,
												  plannode->nullsFirst,
												  work_mem /2,
												  node->randomAccess);
			register_tuplesort_state(tuplesortstate, tuplesortstates[0],0);
			if (node->bounded) {
				tuplesort_set_bound(tuplesortstates[0], node->bound);
			}
			tuplesortstates[1] = tuplesort_begin_heap(tupDesc,
												plannode->numCols,
												  plannode->sortColIdx,
												  plannode->sortOperators,
												  plannode->nullsFirst,
												  work_mem /2,
												  node->randomAccess);
			register_tuplesort_state(tuplesortstate, tuplesortstates[1],1);
			if (node->bounded) {
				tuplesort_set_bound(tuplesortstates[1], node->bound);
			}
		}
		
		
		if (node->bounded)
			tuplesort_set_bound(tuplesortstate, node->bound);
		node->tuplesortstate = (void *) tuplesortstate;

		tuplesort_set_parallel(tuplesortstate, prl_on);
		
		if (!prl_on) {
			ereport(LOG,(errmsg("nodeSort std - before sending tuples to workers")));
			int ii = 0;
			for (;;)
			{
				slot = ExecProcNode(outerNode);
				if (TupIsNull(slot))
					break;
				if (lts) {
					if (ii == 0) {
						tuplesort_puttupleslot(tuplesortstates[0], slot);
						ii = 1;
					} else {
						tuplesort_puttupleslot(tuplesortstates[1], slot);
						ii = 0;
					}
				} else {
					tuplesort_puttupleslot(tuplesortstate, slot);
				}
			}
			
			if (lts) {
				pthread_t threads[2];
				int rc;
				long t;
				for(t=0; t<2; t++){
					rc = pthread_create(&threads[t], NULL, threadedPerformSort, (void *)(tuplesortstates[t]));
					if (rc) {
						printf("ERROR; return code from pthread_create() is %d\n", rc);
					}
				}
				// wait until those lazy workers finish ... 
				for (t = 0; t < 2; t++) {
					rc = pthread_join(threads[t], NULL);
					if (rc) {
						printf("ERROR; return code from pthread_join() is %d\n", rc);
					}
				}
				/*
				ereport(LOG, (errmsg("nodeSort - before 2 performsorts")));
				tuplesort_performsort(tuplesortstates[0]);
				ereport(LOG, (errmsg("nodeSort - after first performsort")));
				tuplesort_performsort(tuplesortstates[1]);
				ereport(LOG, (errmsg("nodeSort - after 2 performsorts")));*/
				preForMergeMultiLTS(tuplesortstate, ScanDirectionIsForward(dir));
				
			} else {
				ereport(LOG, (errmsg("nodeSort std - before performsort")));
				/*
				 * Complete the sort.
				 */
				tuplesort_performsort(tuplesortstate);
				ereport(LOG, (errmsg("nodeSort std - after performsort")));
			}
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
	
			oldContext = MemoryContextSwitchTo(ShmParalellContext);
			
			// kind of on demand but it would be better to do it during init of this backend
			if (workersList == NULL) {
				workersList = createShList();
			}
			
			// prepare work with params and let postmaster create us workers
			for (i=0; i < prl_level; i++) {
				work = (WorkDef*)palloc(sizeof(WorkDef));
				work->workType = PRL_WORK_TYPE_SORT;
				work->state = PRL_STATE_REQUESTED;
				work->workParams = (WorkParams*)palloc(sizeof(WorkParams));
				work->workResult = (WorkResult*)palloc(sizeof(WorkResult));
				work->workParams->dummyValue1 = 100 + 100*i;
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
				work->workParams->workersList = workersList;
				work->workParams->databaseId = MyProc->databaseId;
				work->workParams->roleId = MyProc->roleId;
				work->workParams->username = GetUserNameFromId(MyProc->roleId);
				work->workParams->bufferQueue = createBufferQueue(parallel_shared_queue_size);
				shListAppend(prlJobsList, work);
			}
			
			ereport(LOG,(errmsg("Master: Signalizing Postmaster")));
			SendPostmasterSignal(PMSIGNAL_START_PARALLEL_WORKERS);
			readyCnt = 0;
			
			// wait until they are ready
			waitForWorkers(workersId, prl_level, PRL_WORKER_STATE_INITIAL);
			MemoryContextSwitchTo(oldContext);
			
			registerWorkers(tuplesortstate, prl_level);
			
			// let them know to start working
			stateTransition(workersId, PRL_WORKER_STATE_INITIAL, PRL_WORKER_STATE_READY);
			ereport(LOG,(errmsg("nodeSort - before sending tuples to workers")));
			/*
			 * Scan the subplan and feed all the tuples to tuplesort.
			 */
			for (;;) {
				slot = ExecProcNode(outerNode);
				//++readyCnt;
				if (TupIsNull(slot)) {
					distributeToWorker(tuplesortstate, NULL, true);
					break;
				}
				distributeToWorker(tuplesortstate, slot, false);
			}
			
			printAddUsage();
			
			ereport(LOG,(errmsg("nodeSort - waiting until workers finish the job.")));
			// wait until they finish the job
			waitForWorkers(workersId, prl_level, PRL_WORKER_STATE_FINISHED);
	
			ereport(LOG,(errmsg("nodeSort - workers finished the job.")));
			// get the slaves ready for sending results
			stateTransition(workersId, PRL_WORKER_STATE_FINISHED, PRL_WORKER_STATE_FINISHED_ACK);
			ereport(LOG,(errmsg("nodeSort - workers set to FINISHED_ACK")));
	
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
		
	/*
	 * Get the first or next tuple from tuplesort. Returns NULL if no more
	 * tuples.
	 */
	slot = node->ss.ps.ps_ResultTupleSlot;
	if (prl_on) {
		jobId = tuplesort_get_workersId(tuplesortstate);
//		ereport(DEBUG1,(errmsg("nodeSort - going to fetch first tuple")));
		if (!tuplesort_gettupleslot_from_worker(tuplesortstate, ScanDirectionIsForward(dir), slot)) {
			ereport(LOG,(errmsg("nodeSort - try cleaning")));
			HOLD_INTERRUPTS();
			SpinLockAcquire(&workersList->mutex);
			foreach(lc, workersList->list) {
				worker = (Worker *) lfirst(lc);
				HOLD_INTERRUPTS();
				SpinLockAcquire(&worker->mutex);
				if (worker->valid && worker->state == PRL_WORKER_STATE_FINISHED_ACK && worker->work->jobId == jobId) {
					worker->state = PRL_WORKER_STATE_END;
					ereport(LOG,(errmsg("nodeSort - performing one bufferqueue cleaning")));
					destroyBufferQueue(worker->work->workParams->bufferQueue);
				}
				SpinLockRelease(&worker->mutex);
				RESUME_INTERRUPTS();
			}
			SpinLockRelease(&workersList->mutex);
			RESUME_INTERRUPTS();
			ereport(LOG,(errmsg("nodeSort - finished cleaning")));
			printGetUsage();
		}
	} else {
		if (lts) {
			tuplesort_gettupleslot_from_multiple_lts(tuplesortstate, ScanDirectionIsForward(dir), slot);
		} else {
			(void) tuplesort_gettupleslot(tuplesortstate,
					ScanDirectionIsForward(dir), slot);	
		}
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
	SO1_printf("ExecEndSort: %s\n",
			   "shutting down sort node");

	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(node->ss.ss_ScanTupleSlot);
	/* must drop pointer to sort result tuple */
	ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);

	/*
	 * Release tuplesort resources
	 */
	if (node->tuplesortstate != NULL)
		tuplesort_end((Tuplesortstate *) node->tuplesortstate);
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
