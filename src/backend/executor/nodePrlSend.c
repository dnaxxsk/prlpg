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
	
	ereport(LOG,(errmsg("NodePrlSend - start")));
	
	if (prl_test2) {
		outerNode = outerPlanState(node);
		// testing the speed of buffer queue implementation
		int i = 0;
		slot = ExecProcNode(outerNode);
		
		if (prl_prealloc_queue) {
			ereport(LOG,(errmsg("NodePrlSend - start test - preallocated queue")));
		} else {
			ereport(LOG,(errmsg("NodePrlSend - start test - standard queue")));			
		}
		
		// start counting the time
		gettimeofday(&tv, NULL);
		duration_u = tv.tv_usec;
		duration_s = tv.tv_sec;
		if (TupIsNull(slot)) {
			// wrong
		}
		oldContext = MemoryContextSwitchTo(ShmParallelContext);
		for (i=0; i<prl_test2_cnt; i++) {
			if (!prl_prealloc_queue) {
				bqc = (BufferQueueCell *) palloc(sizeof(BufferQueueCell));
				bqc->last = false;
				bqc->ptr_value = (void *)ExecCopySlotMinimalTuple(slot);
				bufferQueueAdd(bq, bqc, true);
			} else {
				bufferQueueAdd2(bq, slot, false, true);
			}
		}
		
		// send last one
		if (!prl_prealloc_queue) {
			bqc = (BufferQueueCell *) palloc(sizeof(BufferQueueCell));
			bqc->last = true;
			bqc->ptr_value = NULL;
			bufferQueueAdd(bq, bqc, true);
		} else {
			bufferQueueAdd2(bq, NULL, true, true);
		}
		
		// how to get disposed of that "slot" containing the data in a correct way?
		MemoryContextSwitchTo(oldContext);
		gettimeofday(&tv, NULL);
		duration_s = tv.tv_sec - duration_s; 
		duration_u = duration_s * 1000000 + tv.tv_usec - duration_u;
		ereport(LOG,(errmsg("NodePrlSend test %d tuples - end - took %ld.%06ld seconds", prl_test2_cnt, duration_u / 1000000, duration_u % 1000000)));
	} else {
		outerNode = outerPlanState(node);
		for (;;) {
			slot = ExecProcNode(outerNode);
			if (TupIsNull(slot)) {
				// send last one 
				oldContext = MemoryContextSwitchTo(ShmParallelContext);
				if (!prl_prealloc_queue) {
					// master will deallocate it .. 
					bqc = (BufferQueueCell *)palloc(sizeof(BufferQueueCell));
					bqc->last = true;
					bufferQueueAdd(bq, bqc, true);
				} else {
					bufferQueueAdd2(bq,NULL,true,true);
				}
				MemoryContextSwitchTo(oldContext);
				break;
			}
			oldContext = MemoryContextSwitchTo(ShmParallelContext);
			if (!prl_prealloc_queue) {
				bqc = (BufferQueueCell *) palloc(sizeof(BufferQueueCell));
				bqc->last = false;
				bqc->ptr_value = (void *)ExecCopySlotMinimalTuple(slot);
				if (bufferQueueAdd(bq, bqc, true)) {
					// in case of LIMIT stop
					MemoryContextSwitchTo(oldContext);
					break;
				}
			} else {
				if (bufferQueueAdd2(bq, slot,false,true)) {
					// in case of LIMIT stop
					MemoryContextSwitchTo(oldContext);
					break;
				}
			}
			// how to get disposed of that "slot" containing the data in a correct way?
			MemoryContextSwitchTo(oldContext);
		}
		gettimeofday(&tv, NULL);
		duration_s = tv.tv_sec - duration_s; 
		duration_u = duration_s * 1000000 + tv.tv_usec - duration_u;
		ereport(LOG,(errmsg("NodePrlSend - end - took %ld.%06ld seconds", duration_u / 1000000, duration_u % 1000000)));
	}
	
	
	
	return NULL;
}
void ExecEndPrlSend(PrlSendState *node) {
	ExecEndNode(outerPlanState(node));
	return;
}