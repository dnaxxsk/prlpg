/*-------------------------------------------------------------------------
 *
 * nodeAppend.c
 *	  routines to handle append nodes.
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeAppend.c,v 1.77 2010/01/02 16:57:41 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
/* INTERFACE ROUTINES
 *		ExecInitAppend	- initialize the append node
 *		ExecAppend		- retrieve the next tuple from the node
 *		ExecEndAppend	- shut down the append node
 *		ExecReScanAppend - rescan the append node
 *
 *	 NOTES
 *		Each append node contains a list of one or more subplans which
 *		must be iteratively processed (forwards or backwards).
 *		Tuples are retrieved by executing the 'whichplan'th subplan
 *		until the subplan stops returning tuples, at which point that
 *		plan is shut down and the next started up.
 *
 *		Append nodes don't make use of their left and right
 *		subtrees, rather they maintain a list of subplans so
 *		a typical append node looks like this in the plan tree:
 *
 *				   ...
 *				   /
 *				Append -------+------+------+--- nil
 *				/	\		  |		 |		|
 *			  nil	nil		 ...	...    ...
 *								 subplans
 *
 *		Append nodes are currently used for unions, and to support
 *		inheritance queries, where several relations need to be scanned.
 *		For example, in our standard person/student/employee/student-emp
 *		example, where student and employee inherit from person
 *		and student-emp inherits from student and employee, the
 *		query:
 *
 *				select name from person
 *
 *		generates the plan:
 *
 *				  |
 *				Append -------+-------+--------+--------+
 *				/	\		  |		  |		   |		|
 *			  nil	nil		 Scan	 Scan	  Scan	   Scan
 *							  |		  |		   |		|
 *							person employee student student-emp
 */
#include <stdio.h>
#include <string.h>
#include "postgres.h"

#include "miscadmin.h"
#include "executor/execdebug.h"
#include "executor/nodeAppend.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/pmsignal.h"
#include "postmaster/slaveworker.h"
#include "storage/parallel.h"
#include "utils/memutils.h"

static bool exec_append_initialize_next(AppendState *appendstate);
static void registerWorkers(PrlAppendState * state, int prl_level);


/* ----------------------------------------------------------------
 *		exec_append_initialize_next
 *
 *		Sets up the append state node for the "next" scan.
 *
 *		Returns t iff there is a "next" scan to process.
 * ----------------------------------------------------------------
 */
static bool
exec_append_initialize_next(AppendState *appendstate)
{
	int			whichplan;

	/*
	 * get information from the append node
	 */
	whichplan = appendstate->as_whichplan;

	if (whichplan < 0)
	{
		/*
		 * if scanning in reverse, we start at the last scan in the list and
		 * then proceed back to the first.. in any case we inform ExecAppend
		 * that we are at the end of the line by returning FALSE
		 */
		appendstate->as_whichplan = 0;
		return FALSE;
	}
	else if (whichplan >= appendstate->as_nplans)
	{
		/*
		 * as above, end the scan if we go beyond the last scan in our list..
		 */
		appendstate->as_whichplan = appendstate->as_nplans - 1;
		return FALSE;
	}
	else
	{
		return TRUE;
	}
}

/* ----------------------------------------------------------------
 *		ExecInitAppend
 *
 *		Begin all of the subscans of the append node.
 *
 *	   (This is potentially wasteful, since the entire result of the
 *		append node may not be scanned, but this way all of the
 *		structures get allocated in the executor's top level memory
 *		block instead of that of the call to ExecAppend.)
 * ----------------------------------------------------------------
 */
AppendState *
ExecInitAppend(Append *node, EState *estate, int eflags)
{
	AppendState *appendstate = makeNode(AppendState);
	PlanState **appendplanstates;
	int			nplans;
	int			i;
	ListCell   *lc;

	/* check for unsupported flags */
	Assert(!(eflags & EXEC_FLAG_MARK));

	/*
	 * Set up empty vector of subplan states
	 */
	nplans = list_length(node->appendplans);

	appendplanstates = (PlanState **) palloc0(nplans * sizeof(PlanState *));

	/*
	 * create new AppendState for our append node
	 */
	appendstate->ps.plan = (Plan *) node;
	appendstate->ps.state = estate;
	appendstate->appendplans = appendplanstates;
	appendstate->as_nplans = nplans;

	/*
	 * Miscellaneous initialization
	 *
	 * Append plans don't have expression contexts because they never call
	 * ExecQual or ExecProject.
	 */

	/*
	 * append nodes still have Result slots, which hold pointers to tuples, so
	 * we have to initialize them.
	 */
	ExecInitResultTupleSlot(estate, &appendstate->ps);

	/*
	 * call ExecInitNode on each of the plans to be executed and save the
	 * results into the array "appendplans".
	 */
	i = 0;
	foreach(lc, node->appendplans)
	{
		Plan	   *initNode = (Plan *) lfirst(lc);

		appendplanstates[i] = ExecInitNode(initNode, estate, eflags);
		i++;
	}

	/*
	 * initialize output tuple type
	 */
	ExecAssignResultTypeFromTL(&appendstate->ps);
	appendstate->ps.ps_ProjInfo = NULL;

	/*
	 * initialize to scan first subplan
	 */
	appendstate->as_whichplan = 0;
	exec_append_initialize_next(appendstate);

	appendstate->prlInitDone = false;
	return appendstate;
}

/* ----------------------------------------------------------------
 *	   ExecAppend
 *
 *		Handles iteration over multiple subplans.
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecAppend(AppendState *node)
{
	PrlAppendState * pas;
	if (!node->prlInitDone) {
		bool isPrlSqlOn = prl_sql && !isSlaveWorker();
		bool copyPlan = prl_copy_plan;
		int prlSqlLvl = prl_sql_lvl;
		char * prlSqlQ1 = prl_sql_q1; 
		char * prlSqlQ2 = prl_sql_q2;
		pas = (PrlAppendState *) palloc0(sizeof(PrlAppendState));
		pas->prlOn = isPrlSqlOn;
		pas->worksDone = 0;
		node->prl_append_state = (void *) pas;
		
		ereport(DEBUG_PRL1,(errmsg("NodeAppend - start.")));
		
		if (isPrlSqlOn) {
			long int jobId = random();
			int i,j = 0;
			WorkDef* work;
			Worker * worker;
			QueryParams * qp = NULL;
			MemoryContext oldContext;
			ListCell * lc;
			if (ShmMessageContext == NULL) {
				ShmMessageContext = ShmContextCreate(ShmParallelContext,
						"ShmMessageContext", 0, 0, 0);
			}
			oldContext = MemoryContextSwitchTo(ShmMessageContext);
			
			
			pas->jobId = jobId;
			
			if (workersList == NULL) {
				workersList = createShList();
			}
			
			// How many workers we need vs we can ask for
			if (node->as_nplans <= prlSqlLvl) {
				pas->workersCnt = node->as_nplans;
			} else {
				pas->workersCnt = prlSqlLvl;
			}
			pas->worksDone = pas->workersCnt;
			for (i=0; i < pas->workersCnt; ++i) {
				work = (WorkDef*)palloc(sizeof(WorkDef));
				work->new = true;
				work->hasWork = true;
				work->workType = PRL_WORK_TYPE_QUERY;
				work->workParams = (WorkParams*)palloc(sizeof(WorkParams));
				work->jobId = jobId;
				qp = (QueryParams *) palloc(sizeof(QueryParams));
				if (copyPlan) {
					foreach(lc, ((Append *)(node->ps.plan))->appendplans)
					{
						Plan *initNode = (Plan *) lfirst(lc);
						if (j == i) {

							qp->subnode = copyObject(initNode);
							qp->rtable  = copyObject(node->ps.state->es_range_table);
							qp->copyPlan = true;
							break;
						}
						j++;
					}
				} else {
					switch (i) {
					case 0:
						qp->query_string = palloc(strlen(prlSqlQ1));
						qp->copyPlan = false;
						strcpy((char *)qp->query_string, prlSqlQ1);
						break;
					case 1:
						qp->query_string = palloc(strlen(prlSqlQ2));
						qp->copyPlan = false;
						strcpy((char *)qp->query_string, prlSqlQ2);
						break;
					}
				}
				work->workParams->queryParams = qp;
				work->workParams->databaseId = MyProc->databaseId;
				work->workParams->roleId = MyProc->roleId;
				work->workParams->username = GetUserNameFromId(MyProc->roleId);
				work->workParams->bufferQueue
						= createBufferQueue(parallel_shared_queue_size);
				
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
			
			SendPostmasterSignal(PMSIGNAL_START_PARALLEL_WORKERS);
			waitForWorkers(jobId, pas->workersCnt, PRL_WORKER_STATE_INITIAL);	
			MemoryContextSwitchTo(oldContext);
			
			registerWorkers(pas, pas->workersCnt);
			stateTransition(jobId, PRL_WORKER_STATE_INITIAL, PRL_WORKER_STATE_READY);
		}
		
		node->prlInitDone = true;
	}
	pas = (PrlAppendState *) node->prl_append_state;
	
	if (pas->prlOn) {
		int i = pas->lastWorker;
		int j = 0;
		int cycles = 0;
		BufferQueueCell * bqc;
		MinimalTuple tup;
		
		for(;;) {
			bool end = true;
			if (cycles == pas->workersCnt) {
				// we dont want to mindlessly check it all the time 
				cycles = 0;
				pg_usleep(prl_wait_time);
			}
			// check the end
			
			for (j = 0; j < pas->workersCnt; ++j) {
				if (!pas->workersFinished[j]) {
					end = false;
					break;
				}
			}
			if (end) {
				return ExecClearTuple(node->ps.ps_ResultTupleSlot);
			}
			
			// move to next worker
			i = (i+1)% pas->workersCnt;
			// they might have all finished
			if (!pas->workersFinished[i]) {
				bqc = bufferQueueGet(pas->workers[i]->work->workParams->bufferQueue, false);
				if (bqc != NULL) {
					if (bqc->last) {
						
						pfree(bqc);
						if (pas->worksDone < node->as_nplans) {
							int k = 0;
							ListCell * lc;
							MemoryContext oldContext;
							// there are still are some unexecuted plans
							// wait for this worker end its job
							waitForState(pas->workers[i], PRL_WORKER_STATE_END);
							// assign him new job
							SpinLockAcquire(&pas->workers[i]->mutex);
							oldContext = MemoryContextSwitchTo(ShmMessageContext);
							// find correct params
							foreach(lc, ((Append *)(node->ps.plan))->appendplans)
							{
								Plan *initNode = (Plan *) lfirst(lc);
								if (k == pas->worksDone) {
									pas->workers[i]->work->workParams->queryParams->subnode = copyObject(initNode);
									pas->workers[i]->work->workParams->queryParams->rtable = copyObject(node->ps.state->es_range_table);
									pas->workers[i]->work->workParams->queryParams->copyPlan = true;
									break;
								}
								k++;
							}
							pas->worksDone++;
							// set him to run the job
							pas->workers[i]->state = PRL_WORKER_STATE_READY;
							// need to reset the bq stop flag because it was stopped with last one 
							bufferQueueSetStop(pas->workers[i]->work->workParams->bufferQueue, false);
							MemoryContextSwitchTo(oldContext);
							SpinLockRelease(&pas->workers[i]->mutex);
							pas->workersFinished[i] = false;
						} else {
							// if there are not more plans to execute then this worker is finished
							pas->workersFinished[i] = true;
						}
					} else {
						pas->lastWorker = i;
						tup = heap_copy_minimal_tuple(bqc->ptr_value);
						ExecStoreMinimalTuple(tup, node->ps.ps_ResultTupleSlot, true);
						pfree((MinimalTuple)bqc->ptr_value);
						pfree(bqc);
						return node->ps.ps_ResultTupleSlot;
					}
				}
			}
			++cycles;
		}
	} else {
		for (;;)
		{
			PlanState  *subnode;
			TupleTableSlot *result;
	
			/*
			 * figure out which subplan we are currently processing
			 */
			subnode = node->appendplans[node->as_whichplan];
	
			/*
			 * get a tuple from the subplan
			 */
			result = ExecProcNode(subnode);
	
			if (!TupIsNull(result))
			{
				/*
				 * If the subplan gave us something then return it as-is. We do
				 * NOT make use of the result slot that was set up in
				 * ExecInitAppend; there's no need for it.
				 */
				return result;
			}
	
			/*
			 * Go on to the "next" subplan in the appropriate direction. If no
			 * more subplans, return the empty slot set up for us by
			 * ExecInitAppend.
			 */
			if (ScanDirectionIsForward(node->ps.state->es_direction))
				node->as_whichplan++;
			else
				node->as_whichplan--;
			if (!exec_append_initialize_next(node))
				return ExecClearTuple(node->ps.ps_ResultTupleSlot);
	
			/* Else loop back and try to get a tuple from the new subplan */
			ereport(DEBUG_PRL1,(errmsg("NodeAppend - going for next subplan")));
		}
	}
}

/* ----------------------------------------------------------------
 *		ExecEndAppend
 *
 *		Shuts down the subscans of the append node.
 *
 *		Returns nothing of interest.
 * ----------------------------------------------------------------
 */
void
ExecEndAppend(AppendState *node)
{
	PlanState **appendplans;
	int			nplans;
	int			i;
	PrlAppendState * pas;
	ListCell * lc;
	Worker * worker;
	long int jobId = 0;
	int workersCnt = 0;
	bool lastValue = false;
	BufferQueueCell * bqc;
	
	/*
	 * get information from the node
	 */
	appendplans = node->appendplans;
	nplans = node->as_nplans;
	pas = (PrlAppendState *) node->prl_append_state;
	
	for (i = 0; i < nplans; i++)
		ExecEndNode(appendplans[i]);
	
	if (pas->prlOn) {
		jobId = pas->jobId;
		workersCnt = pas->workersCnt;
		SpinLockAcquire(&workersList->mutex);
		foreach(lc, workersList->list) {
			worker = (Worker *) lfirst(lc);
			SpinLockAcquire(&worker->mutex);
			if (worker->valid && worker->state != PRL_WORKER_STATE_END && worker->work->jobId == jobId) {
				lastValue = bufferQueueSetStop(worker->work->workParams->bufferQueue, true);
				if (!lastValue) {
					// clear at least one value, so they have a chance to notice the end
					// sended did not send all tuples ...
					// he might be stuck on semaphore
					// it cant be empty because last one was not put inside
					bqc = bufferQueueGet(worker->work->workParams->bufferQueue, false);
					if (bqc != NULL) {
						if (bqc->last) {
							pfree(bqc);
						} else {
							pfree((MinimalTuple *)bqc->ptr_value);
							pfree(bqc);
						}
					}
				}
			}
			SpinLockRelease(&worker->mutex);
		}
		SpinLockRelease(&workersList->mutex);
		
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
		
		// wait until they sto using shared resources
		waitForWorkers(jobId, workersCnt, PRL_WORKER_STATE_DEAD);

		// release resources
		SpinLockAcquire(&workersList->mutex);
		foreach(lc, workersList->list) {
			worker = (Worker *) lfirst(lc);
			SpinLockAcquire(&worker->mutex);
			if (worker->valid && worker->work->jobId == jobId) {
				ereport(DEBUG_PRL1,(errmsg("nodeAppend - performing one bufferqueue cleaning")));
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
	} 
}

void
ExecReScanAppend(AppendState *node, ExprContext *exprCtxt)
{
	int			i;

	for (i = 0; i < node->as_nplans; i++)
	{
		PlanState  *subnode = node->appendplans[i];

		/*
		 * ExecReScan doesn't know about my subplans, so I have to do
		 * changed-parameter signaling myself.
		 */
		if (node->ps.chgParam != NULL)
			UpdateChangedParamSet(subnode, node->ps.chgParam);

		/*
		 * If chgParam of subnode is not null then plan will be re-scanned by
		 * first ExecProcNode.	However, if caller is passing us an exprCtxt
		 * then forcibly rescan all the subnodes now, so that we can pass the
		 * exprCtxt down to the subnodes (needed for appendrel indexscan).
		 */
		if (subnode->chgParam == NULL || exprCtxt != NULL)
			ExecReScan(subnode, exprCtxt);
	}
	node->as_whichplan = 0;
	exec_append_initialize_next(node);
}

static void registerWorkers(PrlAppendState * state, int prl_level) {
	// register them in my array
	int i, readyCnt = 0;
	Worker * worker;
	ListCell * lc;
	state->workers = palloc(sizeof(Worker *) * prl_level);
	// nobody was last sofar ... but it doesnt really matter who is
	// .. because it will be used as round robin 
	state->lastWorker = -1;

	SpinLockAcquire(&workersList->mutex);
	foreach(lc, workersList->list) {
		worker = (Worker *) lfirst(lc);
		SpinLockAcquire(&worker->mutex);
		if (worker->valid && worker->state == PRL_WORKER_STATE_INITIAL && worker->work->jobId == state->jobId) {
			state->workers[readyCnt++] = worker;
		}
		SpinLockRelease(&worker->mutex);
	}
	SpinLockRelease(&workersList->mutex);

	state->workersCnt = readyCnt;
	Assert(readyCnt == prl_level);
	// set them all as unfinished
	state->workersFinished = (bool *)palloc(sizeof(bool) * state->workersCnt);
	for (i = 0; i < state->workersCnt; ++i) {
		state->workersFinished[i] = false;
	}
}
