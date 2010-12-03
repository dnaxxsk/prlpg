#include "postgres.h"

#include <signal.h>
#include <unistd.h>
#include <sys/time.h>

#include "access/transam.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "postmaster/autovacuum.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/parallel.h"
#include "storage/procarray.h"
#include "storage/spin.h"
#include "storage/pmsignal.h"
#include "utils/memutils.h"
#include "storage/shmem.h"

// global variables
bool prl_sort = false;
int prl_sort_dop = 2;
int parallel_shared_queue_size = 1000;

bool prl_sql = false;
int prl_sql_lvl = 2;
char * prl_sql_q1 = NULL;
char * prl_sql_q2 = NULL;
int prl_wait_time = 1;

bool prl_test = false;
int prl_test_workers = -1;
int prl_test_cycles = -1;
int  prl_test_type = -1;
int  prl_test_chunk_size = -1;
int  prl_test_chunk_cnt = -1;

bool prl_prealloc_queue = false;
int prl_queue_item_size = -1;

bool prl_copy_plan = false;

// requests for spwawning new workers for postmaster
SharedList * prlJobsList;
SharedList * workersToCancel;

// list of workers currently in use by this master process
SharedList * workersList = NULL;

NON_EXEC_STATIC PRL_SEM_HDR *PrlSemGlobal = NULL;

NON_EXEC_STATIC slock_t *PrlSemLock = NULL;

/**
 * Initialization called in postmaster before spawning any backends.
 */
void parallel_init(void) {
	MemoryContext oldContext;
	oldContext = MemoryContextSwitchTo(ShmParallelContext);
	prlJobsList = createShList();
	workersToCancel = createShList();
	MemoryContextSwitchTo(oldContext);
}

SharedList * createShList(void) {
	SharedList * result;
	result = (SharedList*)palloc(sizeof(SharedList));
	SpinLockInit(&result->mutex);
	result->list = NIL;
	return result;
}

void shListAppend(SharedList * list, void * object) {
	HOLD_INTERRUPTS();
	SpinLockAcquire(&list->mutex);
	list->list = lappend(list->list, object);
	SpinLockRelease(&list->mutex);
	RESUME_INTERRUPTS();
}

void shListRemove(SharedList * list, void * object) {
	HOLD_INTERRUPTS();
	SpinLockAcquire(&list->mutex);
	list->list = list_delete_ptr(list->list, object);
	SpinLockRelease(&list->mutex);
	RESUME_INTERRUPTS();
}

void shListRemoveNoLock(SharedList * list, void * object) {
	list->list = list_delete_ptr(list->list, object);
}

void shListAppendInt(SharedList * list, int value) {
	MemoryContext oldContext;
	oldContext = MemoryContextSwitchTo(ShmParallelContext);
	HOLD_INTERRUPTS();
	SpinLockAcquire(&list->mutex);
	list->list = lappend_int(list->list, value);
	SpinLockRelease(&list->mutex);
	MemoryContextSwitchTo(oldContext);
	RESUME_INTERRUPTS();
}

BufferQueue * createBufferQueue(int buffer_size) {
	BufferQueue * bq;
	int i;
	volatile PRL_SEM_HDR * prlSemGlobal = PrlSemGlobal;

	ereport(DEBUG_PRL2,(errmsg("Parallel.c - create buffer queue - start")));
	bq = (BufferQueue *)palloc(sizeof(BufferQueue));
	bq->init_size = buffer_size;
	bq->size = 0;
	bq->stop = false;

	SpinLockAcquire(PrlSemLock);

	if (prlSemGlobal->freeSems != NULL) {
		bq->spaces = prlSemGlobal->freeSems;
		prlSemGlobal->freeSems = bq->spaces->links.next;
	} else {
		ereport(FATAL,
				(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
						errmsg("sorry, too many semaphores used already in parallel execution")));
		SpinLockRelease(PrlSemLock);
	}
	ereport(DEBUG_PRL2,(errmsg("Parallel.c - create buffer queue - spaces created")));

	
	if (prlSemGlobal->freeSems != NULL) {
		bq->items = prlSemGlobal->freeSems;
		prlSemGlobal->freeSems = bq->items->links.next;
	} else {
		ereport(FATAL,
				(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
						errmsg("sorry, too many semaphores used already in parallel execution")));
		SpinLockRelease(PrlSemLock);
	}
	ereport(DEBUG_PRL2,(errmsg("Parallel.c - create buffer queue - items created")));

	if (prlSemGlobal->freeSems != NULL) {
		bq->mutex = prlSemGlobal->freeSems;
		prlSemGlobal->freeSems = bq->mutex->links.next;
	} else {
		ereport(FATAL,
				(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
						errmsg("sorry, too many semaphores used already in parallel execution")));
		SpinLockRelease(PrlSemLock);
	}
	ereport(DEBUG_PRL2,(errmsg("Parallel.c - create buffer queue - mutex created to one")));

	SpinLockRelease(PrlSemLock);

	// initialize to buffer_size
	for (i = 0; i < buffer_size-1; ++i) {
		PGSemaphoreUnlock(&(bq->spaces->sem));
	}
	ereport(DEBUG_PRL2,(errmsg("Parallel.c - create buffer queue - spaces upped to buffer_size")));
	// created to ONE


	// clear to ZERO
	PGSemaphoreLock(&(bq->items->sem), true);
	ereport(DEBUG_PRL2,(errmsg("Parallel.c - create buffer queue - items downed to zero")));
	// created to ONE - that is OK

	bq->head = NULL;
	bq->tail = NULL;
	
	ereport(DEBUG_PRL2,(errmsg("Parallel.c - create buffer queue - end")));
	return bq;
}

void destroyBufferQueue(BufferQueue * bq) {
	volatile PRL_SEM_HDR * prlSemGlobal = PrlSemGlobal;
	ereport(DEBUG_PRL2,(errmsg("Parallel.c - destroy buffer queue")));

	// reset to 0 and unlock to 1 so it is the same as created which inits to one
	PGSemaphoreReset(&(bq->items->sem));
	PGSemaphoreUnlock(&(bq->items->sem));

	PGSemaphoreReset(&(bq->mutex->sem));
	PGSemaphoreUnlock(&(bq->mutex->sem));

	PGSemaphoreReset(&(bq->spaces->sem));
	PGSemaphoreUnlock(&(bq->spaces->sem));

	SpinLockAcquire(PrlSemLock);

	// can link to each other but it is doesnt really matter ...
	bq->items->links.next = prlSemGlobal->freeSems;
	prlSemGlobal->freeSems = bq->items;

	bq->mutex->links.next = prlSemGlobal->freeSems;
	prlSemGlobal->freeSems = bq->mutex;

	bq->spaces->links.next = prlSemGlobal->freeSems;
	prlSemGlobal->freeSems = bq->spaces;

	SpinLockRelease(PrlSemLock);
	
	if (bq->head != NULL ) {
		// problem
		ereport(WARNING, (errcode(ERRCODE_OUT_OF_MEMORY),	errmsg("bufferqueue must be empty in destroy method")));
	}

	pfree(bq);
}

bool bufferQueueAdd(BufferQueue * bq, BufferQueueCell * cell, bool stopOnLast) {
	bool result;
	PGSemaphoreLock(&(bq->spaces->sem), true);
	PGSemaphoreLock(&(bq->mutex->sem), true);
	bq->size++;
	if (bq->tail == NULL) {
		bq->head = cell;
		bq->tail = cell;
		cell->next = NULL;
	} else if (bq->head == bq->tail) {
		bq->tail = cell;
		bq->head->next = bq->tail;
		bq->tail->next = NULL;
	} else {
		(bq->tail)->next = cell;
		bq->tail = cell;
		cell->next = NULL;
	}
	if (stopOnLast && cell->last) {
		bq->stop = true;
	}
	result = bq->stop;
	PGSemaphoreUnlock(&(bq->mutex->sem));
	PGSemaphoreUnlock(&(bq->items->sem));
	return result;
}

BufferQueueCell * bufferQueueGet(BufferQueue * bq, bool wait) {
	BufferQueueCell * result= NULL;
	
	if (!wait) {
		PGSemaphoreLock(&(bq->mutex->sem), true);
		if (bq->size == 0) {
			PGSemaphoreUnlock(&(bq->mutex->sem));
			return NULL;
		}
		PGSemaphoreUnlock(&(bq->mutex->sem));
	}
	PGSemaphoreLock(&(bq->items->sem), true);
	PGSemaphoreLock(&(bq->mutex->sem), true);

	if (bq->head == NULL) {
		return result;
	}

	result = bq->head;
	bq->head = result->next;
	if (result->next == NULL) {
		bq->tail = NULL;
	}
	bq->size--;
	PGSemaphoreUnlock(&(bq->mutex->sem));
	PGSemaphoreUnlock(&(bq->spaces->sem));
	return result;
}

/**
 * Use this only when cleaning and you are sure that no one else can use this
 * cant pfree here because dont know anything about data it is carrying.
 */
BufferQueueCell * bufferQueueGetNoSem(BufferQueue * bq) {
	BufferQueueCell * result= NULL;
	if (bq->head == NULL) {
		return NULL;
	}
	result = bq->head;
	bq->head = result->next;
	return result;
}

/**
 * Initializes semafor used in parallel execution .. they have to be created beforehand and then reused
 */
void InitPrlSemas(void) {
	int i= 0;
	bool found;
	SEM_BOX *boxes;

	PrlSemGlobal = (PRL_SEM_HDR *) ShmemInitStruct("Prl Sem Header",
			sizeof(PRL_SEM_HDR), &found);
	Assert(!found);

	PrlSemGlobal->freeSems = NULL;
	

	boxes = (SEM_BOX *) ShmemAlloc((MaxPrlSems) * sizeof(SEM_BOX));
	if (!boxes)
		ereport(FATAL, (errcode(ERRCODE_OUT_OF_MEMORY),
						errmsg("out of shared memory")));
	MemSet(boxes, 0, MaxPrlSems * sizeof(SEM_BOX));
	for (i = 0; i < MaxPrlSems; i++) {
		PGSemaphoreCreate(&(boxes[i].sem));
		boxes[i].links.next = PrlSemGlobal->freeSems;
		PrlSemGlobal->freeSems = &boxes[i];
	}

	PrlSemLock = (slock_t *) ShmemAlloc(sizeof(slock_t));
	SpinLockInit(PrlSemLock);
}

Size PrlGlobalShmemSize(void) {
	Size size = 0;

	/* PrlSemGlobal */
	size = add_size(size, sizeof(PRL_SEM_HDR));
	/* Semaphore boxes */
	size = add_size(size, mul_size(MaxPrlSems, sizeof(SEM_BOX)));
	/* PrlSemLock */
	size = add_size(size, sizeof(slock_t));

	return size;
}

int PrlGlobalSemas(void) {
	return MaxPrlSems;
}

// returns true when requested number of workers is defined state
bool waitForWorkers(long int jobId, int workersCnt, PRL_WORKER_STATE state) {
	int readyCnt = 0;
	ListCell * lc;
	Worker * worker;
	while (true && readyCnt != workersCnt) {
		readyCnt = 0;
		SpinLockAcquire(&workersList->mutex);
		foreach(lc, workersList->list) {
			worker = (Worker *) lfirst(lc);
			SpinLockAcquire(&worker->mutex);
			if (worker->valid && worker->state == state && worker->work->jobId == jobId) {
				++readyCnt;
			}
			SpinLockRelease(&worker->mutex);
		}
		SpinLockRelease(&workersList->mutex);
		pg_usleep(prl_wait_time);
		CHECK_FOR_INTERRUPTS();
	}
	return true;
}

bool waitForAllWorkers(PRL_WORKER_STATE state) {
	ListCell * lc;
	Worker * worker;
	bool notEnd = true;
	ereport(DEBUG_PRL1,(errmsg("Master: wait for all- start")));
	while (notEnd) {
		notEnd = false;
		SpinLockAcquire(&workersList->mutex);
		foreach(lc, workersList->list) {
			worker = (Worker *) lfirst(lc);
			SpinLockAcquire(&worker->mutex);
			if (worker->valid && (worker->state != state && worker->state != PRL_WORKER_STATE_END)) {
				notEnd = true;
			}
			SpinLockRelease(&worker->mutex);
			pg_usleep(prl_wait_time);
		}
		SpinLockRelease(&workersList->mutex);
	}
	ereport(DEBUG_PRL1,(errmsg("Master: wait for all- end")));
	return true;
}

void cancelWorkers(void) {
	ListCell * lc;
	Worker * worker;
	ereport(DEBUG_PRL1,(errmsg("Master: cancel workers")));

	foreach(lc, workersList->list) {
		worker = (Worker *) lfirst(lc);
		ereport(DEBUG_PRL1,(errmsg("Master: canceling pid ")));
		shListAppendInt(workersToCancel, worker->workerPid);
	}
	
	SendPostmasterSignal(PMSIGNAL_CANCEL_PARALLEL_WORKERS);
	ereport(LOG,(errmsg("Master: cancel workers  - end")));
}

/**
 * copied from postmaster.c
 */
void
signal_child(pid_t pid, int signal)
{
	if (kill(pid, signal) < 0)
		elog(DEBUG3, "kill(%ld,%d) failed: %m", (long) pid, signal);
#ifdef HAVE_SETSID
	switch (signal)
	{
		case SIGINT:
		case SIGTERM:
		case SIGQUIT:
		case SIGSTOP:
			if (kill(-pid, signal) < 0)
				elog(DEBUG3, "kill(%ld,%d) failed: %m", (long) (-pid), signal);
			break;
		default:
			break;
	}
#endif
}

// returns number of workers which changed the state
int stateTransition(long int jobId, PRL_WORKER_STATE oldState,
		PRL_WORKER_STATE newState) {
	int counter = 0;
	ListCell * lc;
	Worker * worker;
	
	SpinLockAcquire(&workersList->mutex);
	foreach(lc, workersList->list) {
		worker = (Worker *) lfirst(lc);
		HOLD_INTERRUPTS();
		SpinLockAcquire(&worker->mutex);
		if (worker->valid && worker->state == oldState && worker->work->jobId == jobId) {
			worker->state = newState;
			counter++;
		}
		SpinLockRelease(&worker->mutex);
		RESUME_INTERRUPTS();
	}
	SpinLockRelease(&workersList->mutex);

	return counter;
}

/**
 * Returns true when achieved. Else 
 */
void waitForState(Worker * worker, PRL_WORKER_STATE state) {
	while (true) {
		SpinLockAcquire(&worker->mutex);
		if (worker->state == state) {
			SpinLockRelease(&worker->mutex);
			break;
		} else if (worker->state == PRL_WORKER_STATE_CANCELED) {
			SpinLockRelease(&worker->mutex);
			ereport(ERROR,(errmsg("Worker - state CANCELLED")));
			break;
		} else {
			SpinLockRelease(&worker->mutex);
		}
		pg_usleep(prl_wait_time);
	}
}

void waitForAndSet(Worker * worker, PRL_WORKER_STATE state, PRL_WORKER_STATE newState) {
	while (true) {
		SpinLockAcquire(&worker->mutex);
		if (worker->state == state) {
			worker->state = newState;
			SpinLockRelease(&worker->mutex);
			break;
		} else if (worker->state == PRL_WORKER_STATE_CANCELED) {
			SpinLockRelease(&worker->mutex);
			ereport(ERROR,(errmsg("Worker - state CANCELLED")));
			break;
		} else {
			SpinLockRelease(&worker->mutex);
		}
		pg_usleep(prl_wait_time);
	}
} 

bool bufferQueueSetStop(BufferQueue * bq, bool newStop) {
	bool result = false;
	PGSemaphoreLock(&(bq->mutex->sem), true);
	result = bq->stop;
	bq->stop = newStop;
	
	PGSemaphoreUnlock(&(bq->mutex->sem));
	return result;
}

void cleanup(void) {
	Worker * worker;
	BufferQueueCell * bqc;
	ListCell * lc;
	foreach(lc, workersList->list) {
		worker = (Worker *) lfirst(lc);
		SpinLockAcquire(&worker->mutex);
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
		destroyBufferQueue(worker->work->workParams->bufferQueue);
		// remove from postmaster work list
		shListRemove(prlJobsList, worker->work);
		// remove from my list 
		shListRemove(workersList, worker);
		SpinLockRelease(&worker->mutex);
	}
}
