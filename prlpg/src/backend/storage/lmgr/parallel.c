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
#include "utils/memutils.h"
#include "storage/shmem.h"

// global variables
bool parallel_execution_allowed = false;
bool parallel_sort_allowed = false;
int parallel_sort_level = 2;
int parallel_shared_queue_size = 5;

// poziadavky na zalozenie novych workerov pre postmastra
SharedList * prlJobsList;

// zoznam workerov ktorych ma k dispozicii tento backend
SharedList * workersList = NULL;

NON_EXEC_STATIC PRL_SEM_HDR *PrlSemGlobal = NULL;

NON_EXEC_STATIC slock_t *PrlSemLock = NULL;

static long int addDuration = 0;
static long int getDuration = 0;

/**
 * Inicializacia volana v postmastri este pred vytvorenim akehokolvek backendu
 */
void parallel_init(void) {
	//MemoryContext oldContext;
	//WorkDef * work;
	//oldContext = MemoryContextSwitchTo(ShmParalellContext);
	//work = (WorkDef*)palloc(sizeof(WorkDef));
	//work->state = -1;

	prlJobsList = createShList();
	// add first dummy because otherwise it would be NIL
	//shListAppend(prlJobsList, work);

	//MemoryContextSwitchTo(oldContext);
}

SharedList * createShList(void) {
	MemoryContext oldContext;
	SharedList * result;
	oldContext = MemoryContextSwitchTo(ShmParalellContext);
	result = (SharedList*)palloc(sizeof(SharedList));
	SpinLockInit(&result->mutex);
	result->list = NIL;
	MemoryContextSwitchTo(oldContext);
	return result;
}

void shListAppend(SharedList * list, void * object) {
	MemoryContext oldContext;
	oldContext = MemoryContextSwitchTo(ShmParalellContext);
	HOLD_INTERRUPTS();
	SpinLockAcquire(&list->mutex);
	list->list = lappend(list->list, object);
	SpinLockRelease(&list->mutex);
	MemoryContextSwitchTo(oldContext);
	RESUME_INTERRUPTS();
}

void shListRemove(SharedList * list, void * object) {
	HOLD_INTERRUPTS();
	SpinLockAcquire(&list->mutex);
	list->list = list_delete_ptr(list->list, object);
	SpinLockRelease(&list->mutex);
	RESUME_INTERRUPTS();
}

BufferQueue * createBufferQueue(int buffer_size) {
	BufferQueue * bq;
	int i;
	volatile PRL_SEM_HDR * prlSemGlobal = PrlSemGlobal;

	ereport(DEBUG1,(errmsg("Parallel.c - create buffer queue - start")));
	bq = (BufferQueue *)palloc(sizeof(BufferQueue));
	bq->init_size = buffer_size;

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
	ereport(DEBUG1,(errmsg("Parallel.c - create buffer queue - spaces created")));

	
	if (prlSemGlobal->freeSems != NULL) {
		bq->items = prlSemGlobal->freeSems;
		prlSemGlobal->freeSems = bq->items->links.next;
	} else {
		ereport(FATAL,
				(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
						errmsg("sorry, too many semaphores used already in parallel execution")));
		SpinLockRelease(PrlSemLock);
	}
	ereport(DEBUG1,(errmsg("Parallel.c - create buffer queue - items created")));

	if (prlSemGlobal->freeSems != NULL) {
		bq->mutex = prlSemGlobal->freeSems;
		prlSemGlobal->freeSems = bq->mutex->links.next;
	} else {
		ereport(FATAL,
				(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
						errmsg("sorry, too many semaphores used already in parallel execution")));
		SpinLockRelease(PrlSemLock);
	}
	ereport(DEBUG1,(errmsg("Parallel.c - create buffer queue - mutex created to one")));

	SpinLockRelease(PrlSemLock);

	// initialize to buffer_size
	for (i = 0; i < buffer_size-1; ++i) {
		PGSemaphoreUnlock(&(bq->spaces->sem));
	}
	ereport(DEBUG1,(errmsg("Parallel.c - create buffer queue - spaces upped to buffer_size")));
	// created to ONE


	// clear to ZERO
	PGSemaphoreLock(&(bq->items->sem), true);
	ereport(DEBUG1,(errmsg("Parallel.c - create buffer queue - items downed to zero")));
	// created to ONE - that is OK

	bq->head = NULL;
	bq->tail = NULL;
	ereport(DEBUG1,(errmsg("Parallel.c - create buffer queue - end")));
	return bq;
}

void destroyBufferQueue(BufferQueue * bq) {
	volatile PRL_SEM_HDR * prlSemGlobal = PrlSemGlobal;
	ereport(DEBUG1,(errmsg("Parallel.c - destroy buffer queue")));

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
	
	if (bq->head != NULL || bq->tail != NULL) {
		// problem
		ereport(WARNING, (errcode(ERRCODE_OUT_OF_MEMORY),	errmsg("bufferqueue must be empty in destroy method")));
	}

	pfree(bq);
}

void bufferQueueAdd(BufferQueue * bq, BufferQueueCell * cell) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	long int duration_u = tv.tv_usec;
	long int duration_s = tv.tv_sec;
//	ereport(DEBUG1,(errmsg("Parallel.c - buffer queue add - start")));
	PGSemaphoreLock(&(bq->spaces->sem), false);
//	ereport(DEBUG1,(errmsg("Parallel.c - buffer queue - spaces downed")));
	PGSemaphoreLock(&(bq->mutex->sem), false);
//	ereport(DEBUG1,(errmsg("Parallel.c - buffer queue - mutex locked")));
	if (bq->tail == NULL) {
//		ereport(DEBUG1,(errmsg("Parallel.c - buffer queue add - was empty")));
		bq->head = cell;
		bq->tail = cell;
		cell->next = NULL;
	} else if (bq->head == bq->tail) {
//		ereport(DEBUG1,(errmsg("Parallel.c - buffer queue add - had just one")));
		bq->tail = cell;
		bq->head->next = bq->tail;
		bq->tail->next = NULL;
	} else {
//		ereport(DEBUG1,(errmsg("Parallel.c - buffer queue add - was not empty")));
		(bq->tail)->next = cell;
		bq->tail = cell;
		cell->next = NULL;
	}
	PGSemaphoreUnlock(&(bq->mutex->sem));
//	ereport(DEBUG1,(errmsg("Parallel.c - buffer queue add - mutex unlocked")));
	PGSemaphoreUnlock(&(bq->items->sem));
	gettimeofday(&tv, NULL);
	duration_s = tv.tv_sec - duration_s;
	duration_u = duration_s * 1000000 + tv.tv_usec - duration_u;
	addDuration += duration_u;
//	ereport(DEBUG1,(errmsg("Parallel.c - buffer queue add - items upped and end")));
}

void printAddUsage(void) {
	ereport(LOG,(errmsg("Parallel.c - buffer queue ADD usage %ld", addDuration) ));
	addDuration = 0;
}

void printGetUsage(void) {
	ereport(LOG,(errmsg("Parallel.c - buffer queue GET usage %ld", getDuration) ));
	getDuration = 0;
}

BufferQueueCell * bufferQueueGet(BufferQueue * bq) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	long int duration_u = tv.tv_usec;
	long int duration_s = tv.tv_sec;
	BufferQueueCell * result= NULL;
//	ereport(DEBUG1,(errmsg("Parallel.c - buffer queue get - start")));
	PGSemaphoreLock(&(bq->items->sem), false);
//	ereport(DEBUG1,(errmsg("Parallel.c - buffer queue get - items downed")));
	PGSemaphoreLock(&(bq->mutex->sem), false);
//	ereport(DEBUG1,(errmsg("Parallel.c - buffer queue get - mutex locked")));

	if (bq->head == NULL) {
		// toto by sa ale nemalo stat kedze nas sem pustil semafor!
//		ereport(DEBUG1,(errmsg("Parallel.c - buffer queue get - getting from empty - something is wrong")));
		gettimeofday(&tv, NULL);
		duration_s = tv.tv_sec - duration_s;
		duration_u = duration_s * 1000000 + tv.tv_usec - duration_u;
		addDuration += duration_u;
		return result;
	}

	result = bq->head;
	bq->head = result->next;
	if (result->next == NULL) {
//		ereport(DEBUG1,(errmsg("Parallel.c - buffer queue get - getting last one")));
		bq->tail = NULL;
	} else {
//		ereport(DEBUG1, (errmsg("Parallel.c - buffer queue get")));
	}

	PGSemaphoreUnlock(&(bq->mutex->sem));
//	ereport(DEBUG1,(errmsg("Parallel.c - buffer queue get - mutex unlocked")));
	PGSemaphoreUnlock(&(bq->spaces->sem));
//	ereport(DEBUG1,(errmsg("Parallel.c - buffer queue get - spaces upped and end")));
	gettimeofday(&tv, NULL);
	duration_s = tv.tv_sec - duration_s;
	duration_u = duration_s * 1000000 + tv.tv_usec - duration_u;
	getDuration += duration_u;
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
	//bool result = false;
	int readyCnt = 0;
	ListCell * lc;
	Worker * worker;
	while (true && readyCnt != workersCnt) {
		readyCnt = 0;
		SpinLockAcquire(&workersList->mutex);
		foreach(lc, workersList->list) {
			worker = (Worker *) lfirst(lc);
			HOLD_INTERRUPTS();
			SpinLockAcquire(&worker->mutex);
			if (worker->valid && worker->state == state && worker->work->jobId == jobId) {
				++readyCnt;
			}
			SpinLockRelease(&worker->mutex);
			RESUME_INTERRUPTS();

		}
		SpinLockRelease(&workersList->mutex);
		pg_usleep(100000L);
	}
	return true;
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


