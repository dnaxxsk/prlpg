#ifndef SLAVEWORKER_H
#define SLAVEWORKER_H

#include "storage/parallel.h"

extern bool isSlaveWorker(void);
extern int slaveBackendMain(WorkDef * work);

#endif