#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodePrlSend.h"
#include "storage/parallel.h"

TupleTableSlot *ExecPrlSend(PrlSendState *node) {
	TupleTableSlot * slot;
	BufferQueue * bq = node->bufferQueue;
	BufferQueueCell * bqc = (BufferQueueCell *)palloc(sizeof(BufferQueueCell));
	PlanState  *outerNode;
	outerNode = outerPlanState(node);
	for (;;) {
		slot = ExecProcNode(outerNode);
		if (TupIsNull(slot)) {
			
		}
		
			
	}
	
	return NULL;
}
void ExecEndPrlSend(PrlSendState *node) {
	return;
}