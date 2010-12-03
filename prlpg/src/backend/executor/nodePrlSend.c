#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodePrlSend.h"
#include "storage/parallel.h"
#include "utils/memutils.h"

TupleTableSlot *ExecPrlSend(PrlSendState *node) {
	TupleTableSlot * slot;
	BufferQueue * bq = node->bufferQueue;
	BufferQueueCell * bqc = NULL;
	PlanState  *outerNode;
	MemoryContext oldContext;
	long int duration_u = 0;
	long int duration_s = 0;
	struct timeval tv;
	gettimeofday(&tv, NULL);
	duration_u = tv.tv_usec;
	duration_s = tv.tv_sec;
	
	ereport(DEBUG_PRL1,(errmsg("NodePrlSend - start")));
	outerNode = outerPlanState(node);
	for (;;) {
		slot = ExecProcNode(outerNode);
		if (TupIsNull(slot)) {
			// send last one 
			oldContext = MemoryContextSwitchTo(ShmParallelContext);
			// master will deallocate it .. 
			bqc = (BufferQueueCell *)palloc(sizeof(BufferQueueCell));
			bqc->last = true;
			bufferQueueAdd(bq, bqc, true);
			MemoryContextSwitchTo(oldContext);
			break;
		}
		oldContext = MemoryContextSwitchTo(ShmParallelContext);
		bqc = (BufferQueueCell *) palloc(sizeof(BufferQueueCell));
		bqc->last = false;
		bqc->ptr_value = (void *)ExecCopySlotMinimalTuple(slot);
		if (bufferQueueAdd(bq, bqc, true)) {
			// in case of LIMIT stop
			MemoryContextSwitchTo(oldContext);
			break;
		}
		MemoryContextSwitchTo(oldContext);
	}
	gettimeofday(&tv, NULL);
	duration_s = tv.tv_sec - duration_s; 
	duration_u = duration_s * 1000000 + tv.tv_usec - duration_u;
	ereport(DEBUG_PRL1,(errmsg("NodePrlSend - end - took %ld.%06ld seconds", duration_u / 1000000, duration_u % 1000000)));
	
	return NULL;
}
void ExecEndPrlSend(PrlSendState *node) {
	ExecEndNode(outerPlanState(node));
	return;
}
