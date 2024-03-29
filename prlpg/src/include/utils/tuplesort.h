/*-------------------------------------------------------------------------
 *
 * tuplesort.h
 *	  Generalized tuple sorting routines.
 *
 * This module handles sorting of heap tuples, index tuples, or single
 * Datums (and could easily support other kinds of sortable objects,
 * if necessary).  It works efficiently for both small and large amounts
 * of data.  Small amounts are sorted in-memory using qsort().	Large
 * amounts are sorted using temporary files and a standard external sort
 * algorithm.
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/utils/tuplesort.h,v 1.35 2010/01/02 16:58:10 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPLESORT_H
#define TUPLESORT_H

#include "access/itup.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "utils/relcache.h"
#include "storage/parallel.h"


/* Tuplesortstate is an opaque type whose details are not known outside
 * tuplesort.c.
 */
typedef struct Tuplesortstate Tuplesortstate;

/*
 * We provide two different interfaces to what is essentially the same
 * code: one for sorting HeapTuples and one for sorting IndexTuples.
 * They differ primarily in the way that the sort key information is
 * supplied.  Also, the HeapTuple case actually stores MinimalTuples,
 * which means it doesn't preserve the "system columns" (tuple identity and
 * transaction visibility info).  The IndexTuple case does preserve all
 * the header fields of an index entry.  In the HeapTuple case we can
 * save some cycles by passing and returning the tuples in TupleTableSlots,
 * rather than forming actual HeapTuples (which'd have to be converted to
 * MinimalTuples).
 *
 * The IndexTuple case is itself broken into two subcases, one for btree
 * indexes and one for hash indexes; the latter variant actually sorts
 * the tuples by hash code.  The API is the same except for the "begin"
 * routine.
 *
 * Yet another slightly different interface supports sorting bare Datums.
 */

extern Tuplesortstate *tuplesort_begin_heap(TupleDesc tupDesc,
					 int nkeys, AttrNumber *attNums,
					 Oid *sortOperators, bool *nullsFirstFlags,
					 int workMem, bool randomAccess);
extern Tuplesortstate *tuplesort_begin_index_btree(Relation indexRel,
							bool enforceUnique,
							int workMem, bool randomAccess);
extern Tuplesortstate *tuplesort_begin_index_hash(Relation indexRel,
						   uint32 hash_mask,
						   int workMem, bool randomAccess);
extern Tuplesortstate *tuplesort_begin_datum(Oid datumType,
					  Oid sortOperator, bool nullsFirstFlag,
					  int workMem, bool randomAccess);

extern void tuplesort_set_bound(Tuplesortstate *state, int64 bound);
extern void tuplesort_set_prl_level(Tuplesortstate * state, int level);
extern int  tuplesort_get_prl_level(Tuplesortstate * state);
extern void tuplesort_set_workersId(Tuplesortstate * state, long int id);
extern long int tuplesort_get_workersId(Tuplesortstate * state);
extern void tuplesort_set_parallel(Tuplesortstate * state, bool prl_on);
extern bool tuplesort_is_parallel(Tuplesortstate * state);

extern void tuplesort_puttupleslot(Tuplesortstate *state,
					   TupleTableSlot *slot);
extern void tuplesort_putindextuple(Tuplesortstate *state, IndexTuple tuple);
extern void tuplesort_putdatum(Tuplesortstate *state, Datum val,
				   bool isNull);

extern void tuplesort_performsort(Tuplesortstate *state);

extern bool tuplesort_gettupleslot(Tuplesortstate *state, bool forward,
					   TupleTableSlot *slot);
extern IndexTuple tuplesort_getindextuple(Tuplesortstate *state, bool forward,
						bool *should_free);
extern bool tuplesort_getdatum(Tuplesortstate *state, bool forward,
				   Datum *val, bool *isNull);

extern void tuplesort_end(Tuplesortstate *state);

extern void tuplesort_get_stats(Tuplesortstate *state,
								const char **sortMethod,
								const char **spaceType,
								long *spaceUsed);

extern int	tuplesort_merge_order(long allowedMem);

/*
 * These routines may only be called if randomAccess was specified 'true'.
 * Likewise, backwards scan in gettuple/getdatum is only allowed if
 * randomAccess was specified.
 */

extern void tuplesort_rescan(Tuplesortstate *state);
extern void tuplesort_markpos(Tuplesortstate *state);
extern void tuplesort_restorepos(Tuplesortstate *state);

/* Setup for ApplySortFunction */
extern void SelectSortFunction(Oid sortOperator, bool nulls_first,
				   Oid *sortFunction,
				   int *sortFlags);

/*
 * Apply a sort function (by now converted to fmgr lookup form)
 * and return a 3-way comparison result.  This takes care of handling
 * reverse-sort and NULLs-ordering properly.
 */
extern int32 ApplySortFunction(FmgrInfo *sortFunction, int sortFlags,
				  Datum datum1, bool isNull1,
				  Datum datum2, bool isNull2);
/*
 * Used in paralell processing
 */
// used in master to copy tuple to shared memory to be sent to slave
extern PrlSortTuple * tuplesort_prl_puttupleslot(Tuplesortstate * state, TupleTableSlot * slot);
// used in slave to put tuple into sorting process
extern void tuplesort_puttupleslot_prl(Tuplesortstate *state, PrlSortTuple *slot);
// used in slave for retrieving tuples from sorting process to shared memory to be sent to master
extern bool prl_tuplesort_getsorttuple(Tuplesortstate * state, bool forward, PrlSortTuple * pstup);
// used in master to use tuples from slaves
extern bool tuplesort_gettupleslot_from_worker(Tuplesortstate * state, bool forward, TupleTableSlot * slot);

extern void registerWorkers(Tuplesortstate * state, int prl_level);

extern void distributeToWorker(Tuplesortstate * state, TupleTableSlot * slot, bool last);

extern void prepareForMerge(Tuplesortstate * state);


/**
 * Methods supporting multiple tapeset sorting engine
 */
extern void prepare_for_multiLTS(Tuplesortstate * state, int cnt);

extern void register_tuplesort_state(Tuplesortstate * state, Tuplesortstate * worker_state, int position);

/**
 * Fetch from each tapeset one tuple and place it to our heap
 */
extern void preForMergeMultiLTS(Tuplesortstate * state, bool forward);

extern bool tuplesort_gettupleslot_from_multiple_lts(Tuplesortstate * state, bool forward, TupleTableSlot * slot);
extern void copyLtsToSharedMem(Tuplesortstate * state);

#endif   /* TUPLESORT_H */
