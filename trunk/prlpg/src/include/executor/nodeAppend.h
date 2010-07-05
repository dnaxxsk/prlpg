/*-------------------------------------------------------------------------
 *
 * nodeAppend.h
 *
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/executor/nodeAppend.h,v 1.30 2010/01/02 16:58:03 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEAPPEND_H
#define NODEAPPEND_H

#include "nodes/execnodes.h"
#include "storage/parallel.h"

extern AppendState *ExecInitAppend(Append *node, EState *estate, int eflags);
extern TupleTableSlot *ExecAppend(AppendState *node);
extern void ExecEndAppend(AppendState *node);
extern void ExecReScanAppend(AppendState *node, ExprContext *exprCtxt);

typedef struct PrlAppendState
{
	bool prlOn;
	int workersCnt;
	int lastWorker;
	Worker ** workers;
	bool * workersFinished;
	long int jobId;
} PrlAppendState;

#endif   /* NODEAPPEND_H */
