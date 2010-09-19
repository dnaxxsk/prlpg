#include "postgres.h"
#include "utils/memutils.h"
#include "storage/spin.h"
#include "miscadmin.h"
#include "mm.h"

#define ALLOC_BLOCKHDRSZ	MAXALIGN(sizeof(AllocBlockData))
#define ALLOC_CHUNKHDRSZ	MAXALIGN(sizeof(AllocChunkData))

typedef struct AllocBlockData * AllocBlock;
typedef struct AllocChunkData * AllocChunk;

typedef struct BlockBox {
	// protection of parallel updates to list :-(
	slock_t mutex;
	// head .. so prev of this is NULL
	AllocBlock blocks;
} BlockBox;

typedef struct ShmContext {
	MemoryContextData header; /* Standard memory-context fields */
	/* Info about storage allocated in this context: */
	MM * mm;
	bool isTop;
	//AllocBlock blocks;
	BlockBox * box;
} ShmContext;

typedef ShmContext* ShmCtx;

typedef struct AllocBlockData {
	ShmCtx sctx;
//	Size size;
	AllocBlock next;
	AllocBlock prev;
} AllocBlockData;

typedef struct AllocChunkData
{
	/* aset is the owning aset if allocated, or the freelist link if free */
	void	   *sctx;
	/* size is always the size of the usable space in the chunk */
	Size		size;
#ifdef MEMORY_CONTEXT_CHECKING
	/* when debugging memory usage, also store actual requested size */
	/* this is zero in a free chunk */
	Size		requested_size;
#endif
} AllocChunkData;

typedef void *AllocPointer;

#define AllocPointerGetChunk(ptr)	\
					((AllocBlock)(((char *)(ptr)) - ALLOC_CHUNKHDRSZ))
#define AllocChunkGetPointer(chk)	\
					((AllocPointer)(((char *)(chk)) + ALLOC_CHUNKHDRSZ))

/*
 typedef struct MemoryContextMethods
 {
 void	   *(*alloc) (MemoryContext context, Size size);
 // call this free_p in case someone #define's free() 
 void		(*free_p) (MemoryContext context, void *pointer);
 void	   *(*realloc) (MemoryContext context, void *pointer, Size size);
 void		(*init) (MemoryContext context);
 void		(*reset) (MemoryContext context);
 void		(*delete) (MemoryContext context);
 Size		(*get_chunk_space) (MemoryContext context, void *pointer);
 bool		(*is_empty) (MemoryContext context);
 void		(*stats) (MemoryContext context, int level);

 } MemoryContextMethods;

 
 */

static void * ShmAlloc(MemoryContext context, Size size);
static void ShmFreeAlloc(MemoryContext context, void * pointer);
static void * ShmRealloc(MemoryContext context, void * pointer, Size size);
static void ShmInit(MemoryContext context);
static void ShmReset(MemoryContext context);
static void ShmDelete(MemoryContext context);
static Size ShmGetChunkSpace(MemoryContext context, void * pointer);
static bool ShmIsEmpty(MemoryContext context);
static void ShmStats(MemoryContext context, int level);

static MemoryContextMethods ShmAllocMethods = { ShmAlloc, ShmFreeAlloc,
		ShmRealloc, ShmInit, ShmReset, ShmDelete, ShmGetChunkSpace, ShmIsEmpty,
		ShmStats };

MemoryContext ShmContextCreate(MemoryContext parent, const char * name,
		Size minContextSize, Size initBlockSize, Size maxBlockSize) {
	ShmCtx shmCtx;

	// prepare standard context attributes
	shmCtx = (ShmCtx) MemoryContextCreate(T_ShmContext, sizeof(ShmContext),
			&ShmAllocMethods, parent, name);

	// top node or next child?
	if (parent == NULL) {
		char * mmAreaName = "magicString";
		unsigned long int sizeOfArea = 1024*1024*200;
		shmCtx->mm = mm_create(sizeOfArea, mmAreaName);
		shmCtx->isTop = true;
	} else {
		shmCtx->mm = ((ShmCtx)parent)->mm;
		shmCtx->isTop = false;
	}
	
	shmCtx->box = (BlockBox *) mm_malloc(shmCtx->mm, sizeof(BlockBox));
	shmCtx->box->blocks = NULL;
	
	SpinLockInit(&shmCtx->box->mutex);

	return (MemoryContext) shmCtx;
}

/**
 * We allocate
 * 
 */
static void * ShmAlloc(MemoryContext context, Size size) {
	ShmCtx ctx = (ShmCtx) context;
	AllocBlock block = (AllocBlock) mm_malloc(ctx->mm, size + ALLOC_BLOCKHDRSZ + ALLOC_CHUNKHDRSZ);
	if (block == NULL) {
		// probably not enough memory
		// TODO report it in ereport?
		ereport(WARNING,(errmsg("mm_malloc failed .. probably not wenough shared memory")));
		return NULL;
	}
	//ereport(LOG,(errmsg("cheerfull alloc %p", block)));
	block->sctx = ctx;
	//block->size = size;
	block->prev = NULL;
	AllocChunk chunk = (AllocChunk) ((char *)block +  ALLOC_BLOCKHDRSZ);
	chunk->sctx = ctx;
	chunk->size = size;
	HOLD_INTERRUPTS();
	SpinLockAcquire(&ctx->box->mutex);
	if (ctx->box->blocks == NULL) {
		block->next = NULL;
	} else {
		block->next = ctx->box->blocks;
		ctx->box->blocks->prev = block;
	}
	ctx->box->blocks = block;
	SpinLockRelease(&ctx->box->mutex);
	RESUME_INTERRUPTS();
	
	return AllocChunkGetPointer(chunk);
}

static void ShmFreeAlloc(MemoryContext context, void * pointer) {
	ShmCtx ctx = (ShmCtx) context;
	AllocChunk chunk = AllocPointerGetChunk(pointer);
	AllocBlock block = (AllocBlock)((char *)chunk - ALLOC_BLOCKHDRSZ);
	AllocBlock b = NULL;
	HOLD_INTERRUPTS();
	SpinLockAcquire(&ctx->box->mutex);
	b = ctx->box->blocks;
	// head of list 
	if (b == block) {
		if (block->next != NULL) {
			// it is not alone
			ctx->box->blocks = block->next;
			ctx->box->blocks->prev = NULL;
		} else {
			ctx->box->blocks = NULL;
		}	
	} else {
		block->prev->next = block->next;
		if (block->next != NULL) {
			block->next->prev = block->prev;
		}
	}
	SpinLockRelease(&ctx->box->mutex);
	RESUME_INTERRUPTS();
	
	mm_free(ctx->mm,(void *)block);
	
	return;
}

// Toto nemoze fungovat ...
// pointer je obaleny AllocBlokom ... 
static void * ShmRealloc(MemoryContext context, void * pointer, Size size) {
	ShmCtx ctx = (ShmCtx) context;
	
	if (pointer == NULL) {
		return mm_malloc(ctx->mm, size);
	} else {
		mm_free(ctx->mm, pointer);
	}
}

static void ShmInit(MemoryContext context) {
	// empty
}

// Dalsia velmi pochybna metoda ...
static void ShmReset(MemoryContext context) {
	ShmCtx ctx = (ShmCtx) context;
	ereport(FATAL, (errmsg("NOT IMPLEMENTED!")));
	if (context->parent == NULL) {
		mm_reset(ctx->mm);
	}
}

static void ShmDelete(MemoryContext context) {
	ShmCtx ctx = (ShmCtx) context;
	AllocBlock block = ctx->box->blocks;
	while (block != NULL) {
		AllocBlock next = block->next;
		
		mm_free(ctx->mm, (void *)block);
		
		block = next;
	}
	
	mm_free(ctx->mm, ctx->box);
	
	if (context->parent == NULL) {
		mm_destroy(ctx->mm);
	}
}

static Size ShmGetChunkSpace(MemoryContext context, void * pointer) {
	AllocChunk	chunk = AllocPointerGetChunk(pointer);
	return chunk->size + ALLOC_BLOCKHDRSZ + ALLOC_CHUNKHDRSZ;
}

static bool ShmIsEmpty(MemoryContext context) {
	// no method in api
	return false;
}

/**
 * Prints statistics from mm module, so we have now no control of it.
 */
static void ShmStats(MemoryContext context, int level) {
	ShmCtx ctx = (ShmCtx) context; 
	mm_display_info(ctx->mm);
}
