/*-------------------------------------------------------------------------
 *
 * walsender.h
 *	  Exports from replication/walsender.c.
 *
 * Portions Copyright (c) 2010-2010, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/include/replication/walsender.h,v 1.1 2010/01/15 09:19:09 heikki Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef _WALSENDER_H
#define _WALSENDER_H

#include "access/xlog.h"
#include "storage/spin.h"

/*
 * Each walsender has a WalSnd struct in shared memory.
 */
typedef struct WalSnd
{
	pid_t	pid;		/* this walsender's process id, or 0 */
	XLogRecPtr sentPtr;	/* WAL has been sent up to this point */

	slock_t	mutex;		/* locks shared variables shown above */
} WalSnd;

/* There is one WalSndCtl struct for the whole database cluster */
typedef struct
{
	WalSnd	walsnds[1];		/* VARIABLE LENGTH ARRAY */
} WalSndCtlData;

extern WalSndCtlData *WalSndCtl;

/* global state */
extern bool	am_walsender;

/* user-settable parameters */
extern int	WalSndDelay;

extern int WalSenderMain(void);
extern void WalSndSignals(void);
extern Size WalSndShmemSize(void);
extern void WalSndShmemInit(void);
extern XLogRecPtr GetOldestWALSendPointer(void);

#endif	/* _WALSENDER_H */
