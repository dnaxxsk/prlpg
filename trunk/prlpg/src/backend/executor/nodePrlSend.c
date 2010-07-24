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
		// how to get disposed of that "slot" containing the data in a correct way?
		MemoryContextSwitchTo(oldContext);
	}
	
	return NULL;
}
void ExecEndPrlSend(PrlSendState *node) {
	ExecEndNode(outerPlanState(node));
	return;
}