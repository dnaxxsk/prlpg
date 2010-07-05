#ifndef NODEPRLSEND_H
#define NODEPRLSEND_H

#include "nodes/execnodes.h"

extern TupleTableSlot *ExecPrlSend(PrlSendState *node);
extern void ExecEndPrlSend(PrlSendState *node);


#endif   /* NODEPRLSEND_H */