#ifndef _PARALLEL_H_
#define _PARALLEL_H_

#include "storage/lock.h"
#include "storage/spin.h"
#include "nodes/plannodes.h"

extern bool prl_sort;
extern int  prl_sort_dop;
extern int  parallel_shared_queue_size;
extern int  prl_wait_time;
extern int  prl_queue_item_size;

extern bool prl_sql;
extern int prl_sql_lvl;
extern char * prl_sql_q1;
extern char * prl_sql_q2;

// params for testing
extern bool prl_test;
extern int 	prl_test_workers;
extern int 	prl_test_cycles;
extern int  prl_test_type;
extern int  prl_test_chunk_size;
extern int  prl_test_chunk_cnt;

extern bool prl_prealloc_queue;
extern int prl_queue_item_size;

extern bool prl_copy_plan;

typedef enum {
	PRL_WORKER_STATE_NONE, // initial state set in master before request send to postmaster
	PRL_WORKER_STATE_INITIAL, // proces caka na pracu, napr ihned po forknuti
	PRL_WORKER_STATE_READY, // master mu nastavi pracu
	PRL_WORKER_STATE_WORKING, // worker si vsimol ze ma pracu a pracuje
	PRL_WORKER_STATE_FINISHED, // worker uz dopracoval
	PRL_WORKER_STATE_FINISHED_ACK, // master/konzument si vsimol ze worker uz dopracoval
	PRL_WORKER_STATE_END, // worker sa ukoncil
	PRL_WORKER_STATE_CANCELED, //
	PRL_WORKER_STATE_DIED
} PRL_WORKER_STATE;

typedef enum {
	PRL_WORK_TYPE_SORT
	,PRL_WORK_TYPE_QUERY
	,PRL_WORK_TYPE_TEST
	,PRL_WORK_TYPE_END
} PRL_WORK_TYPE;

//typedef struct SharedList SharedList;
typedef struct SharedList 
{
	List * list;
	slock_t mutex;
} SharedList;


typedef struct WorkParams
{
	struct SortParams * sortParams;
	struct QueryParams * queryParams;
	struct TestParams * testParams;
	struct BufferQueue * bufferQueue;
	Oid databaseId;
	Oid roleId;
	char * username;
} WorkParams;

/**
 * This structure is not being used sofar
 */
typedef struct WorkResult
{
	int dummyResult1;
} WorkResult;


typedef struct Worker
{
	bool valid;
	pid_t workerPid;
	struct WorkDef * work;
	// protektor
	slock_t mutex;
	PRL_WORKER_STATE state;
} Worker;

typedef struct WorkDef
{
	bool new;
	bool hasWork;
	PRL_WORK_TYPE workType;
	WorkParams * workParams;
	WorkResult * workResult;
	pid_t masterPid;
	Worker * worker;
	long int jobId;
} WorkDef;

typedef struct SortParams {
	// tieto su potrebne v tuple_sort_begin_heap
	TupleDesc	tupDesc; // created by existing copy function 
	int			numCols;
	AttrNumber *sortColIdx; // numCols je velkost tychto poli
	Oid		   *sortOperators;
	bool	   *nullsFirst;
	int			work_mem; // toto zrejme nie, pretoze slave ma vlastnu velkost pamate
	bool		randomAccess;	/* need random access to sort output? */
	// tuple_sort_set_bounded
	bool		bounded;		/* is the result set bounded? */
	bool 		forward;
	int64		bound;
} SortParams;

typedef struct QueryParams {
	// exec simple query
	const char *query_string;
	Plan  *subnode;
	List  *rtable;
	bool copyPlan;
} QueryParams;


typedef struct TestParams {
	// parameters of test 
	int			type;
	int			cycles;
	int			chunk_size;
	int			chunk_cnt;
} TestParams;

// allocated in shared memory
extern List * workDefList;
extern SharedList * prlJobsList;
extern SharedList * workersList;
extern SharedList * workersToCancel;

extern void parallel_init(void);

//extern bool prepareSlaves(int i);

extern SharedList * createShList(void);
extern void shListAppend(SharedList * list, void * object);
extern void shListRemove(SharedList * list, void * object);
extern void shListAppendInt(SharedList * list, int value);
extern void shListRemoveNoLock(SharedList * list, void * object);

typedef struct BufferQueueCell BufferQueueCell;

typedef struct BufferQueue
{
	BufferQueueCell   *head;
	BufferQueueCell   *tail;
	struct SEM_BOX * items;
	struct SEM_BOX * spaces;
	struct SEM_BOX * mutex;
	int init_size;
	int size;
	bool stop;
	int first;
	int next;
	void ** data;
} BufferQueue;

struct BufferQueueCell
{
	void	   *ptr_value;
	BufferQueueCell   *next;
	bool last;
	int size;
};

extern BufferQueue * createBufferQueue(int buffer_size);
extern void destroyBufferQueue(BufferQueue * bq);
extern bool bufferQueueAdd(BufferQueue * bq, BufferQueueCell * cell, bool stopOnLast);
extern BufferQueueCell * bufferQueueGet(BufferQueue * bq, bool wait);
extern BufferQueueCell * bufferQueueGetNoSem(BufferQueue * bq);
extern bool bufferQueueSetStop(BufferQueue * bq, bool newStop);

typedef struct SEM_BOX {
	SHM_QUEUE	links;
	PGSemaphoreData sem;
} SEM_BOX;

typedef struct PRL_SEM_HDR {
	SEM_BOX * freeSems; // head of list of free semaphores
} PRL_SEM_HDR;

typedef struct
{
	void	   *tuple;			/* the tuple proper */
	Datum		datum1;			/* value of first key column */
	bool		isnull1;		/* is first key column NULL? */
	int			tupindex;		/* see notes above */
} PrlSortTuple;

extern void InitPrlSemas(void);
extern int	PrlGlobalSemas(void);
extern Size PrlGlobalShmemSize(void);

// -------------------------
// workers state transitions
// -------------------------

// returns true when requested number of workers is defined state
extern bool waitForWorkers(long int jobId, int workersCnt, PRL_WORKER_STATE state);

extern bool waitForAllWorkers(PRL_WORKER_STATE state);
extern void waitForState(Worker * worker, PRL_WORKER_STATE state);
extern void waitForAndSet(Worker * worker, PRL_WORKER_STATE state, PRL_WORKER_STATE newState);
extern void cancelWorkers(void);

// returns number of workers which changed the state
extern int stateTransition(long int jobId, PRL_WORKER_STATE oldState, PRL_WORKER_STATE newState);

//extern void printAddUsage(void);
//extern void printGetUsage(void);

extern void cleanup(void);
#endif