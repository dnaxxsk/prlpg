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
			oldContext = MemoryContextSwitchTo(ShmParalellContext);
			// master will deallocate it .. 
			bqc = (BufferQueueCell *)palloc(sizeof(BufferQueueCell));
			bqc->last = true;
			bufferQueueAdd(bq, bqc, false);
			MemoryContextSwitchTo(oldContext);
			break;
		}
		// TODO send all
	}
	
	return NULL;
}
void ExecEndPrlSend(PrlSendState *node) {
	
	ExecEndNode(outerPlanState(node));
	return;
}