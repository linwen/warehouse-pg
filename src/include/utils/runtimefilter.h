/*-------------------------------------------------------------------------
 *
 * runtimefilter.h
 *	  Runtime filters for hash joins: a filter over the inner side's join
 *	  keys, handed to the probe side's scan in the same slice.
 *
 * See runtimefilter.c for the filter and the probe function, planrtfilter.c
 * for the planner pass that decides where one is used.
 *
 * Portions Copyright (c) 2026, WarehousePG contributors.
 *
 * src/include/utils/runtimefilter.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RUNTIMEFILTER_H
#define RUNTIMEFILTER_H

#include "access/hash.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "utils/palloc.h"

/* GUCs, see guc_gp.c */
extern bool whpg_enable_runtime_filter;
extern int	whpg_runtime_filter_max_size;	/* kB, one filter */
extern double whpg_runtime_filter_max_selectivity;
extern int	whpg_runtime_filter_min_probe_rows;
extern double whpg_runtime_filter_disable_ratio;
extern bool whpg_runtime_filter_explain;

/*
 * Building a filter from the inner side's hash values, one insert per
 * tuple the hash table takes (nodeHash.c), published by the join
 * (nodeHashjoin.c) once the table is complete.
 */
typedef struct RuntimeFilterBuilder RuntimeFilterBuilder;

extern bool runtimefilter_fits(double nkeys);
extern RuntimeFilterBuilder *runtimefilter_create(double est_nkeys,
												  MemoryContext cxt);
extern void runtimefilter_insert(RuntimeFilterBuilder *builder,
								 uint32 hashvalue);
extern struct varlena *runtimefilter_finish(RuntimeFilterBuilder *builder);
extern bool runtimefilter_merge(struct varlena *into,
								const struct varlena *other);
extern struct varlena *runtimefilter_check(struct varlena *v);

/*
 * The filter itself, as the bytea holds it, and the probe -- inline, so that
 * the AOCO Staged Scan can test a block's raw datum array against it without
 * going through fmgr (aocs_stagedscan.c), exactly as whpg_rf_match() does.
 *
 * A split-block Bloom filter: 256-bit blocks of eight 32-bit words, one bit
 * set per word (k = 8), so a probe touches one cache line. The 32-bit hash
 * value is the hash table's own; it is spread to 64 bits, the top half picks
 * the block, the bottom half the bit in each word through eight odd
 * multipliers.
 */
#define RF_MAGIC				0x32465257	/* "WRF2": format and version */
#define RF_BLOCK_WORDS			8
#define RF_BLOCK_BITS			(RF_BLOCK_WORDS * 32)

typedef struct RuntimeFilter
{
	int32		vl_len_;		/* varlena header (do not touch directly) */
	uint32		magic;
	uint32		log2_blocks;
	uint32		nkeys;			/* keys inserted, saturating */
	uint32		flags;			/* none defined yet; equal on both sides of a merge */

	/*
	 * The switch-off's tally (runtimefilter_probe_count() below), kept in
	 * the filter so that everything that probes it in this process -- the
	 * scan's qual, a Staged Scan's fast path, every partition's scan under a
	 * dynamic scan -- counts into one tally and gives the filter up together.
	 * State of the process that probes, not content: a fresh filter starts
	 * at zero, a merge leaves it alone.
	 */
	uint32		disabled;
	uint64		seen;
	uint64		passed;

	uint32		blocks[FLEXIBLE_ARRAY_MEMBER];	/* RF_BLOCK_WORDS per block */
} RuntimeFilter;

extern const uint32 runtimefilter_salt[RF_BLOCK_WORDS];

static inline uint64
runtimefilter_mix64(uint32 hashvalue)
{
	uint64		x = hashvalue;

	x ^= x >> 33;
	x *= UINT64CONST(0xff51afd7ed558ccd);
	x ^= x >> 33;
	x *= UINT64CONST(0xc4ceb9fe1a85ec53);
	x ^= x >> 33;
	return x;
}

static inline uint32 *
runtimefilter_block(RuntimeFilter *f, uint64 h)
{
	uint32		blk = ((uint32) (h >> 32)) & ((1U << f->log2_blocks) - 1);

	return f->blocks + (Size) blk * RF_BLOCK_WORDS;
}

static inline bool
runtimefilter_contains(RuntimeFilter *f, uint32 hashvalue)
{
	uint64		h = runtimefilter_mix64(hashvalue);
	const uint32 *block = runtimefilter_block(f, h);
	uint32		hm = (uint32) h;

	for (int i = 0; i < RF_BLOCK_WORDS; i++)
	{
		if ((block[i] & (1U << ((hm * runtimefilter_salt[i]) >> 27))) == 0)
			return false;
	}
	return true;
}

/*
 * The hash value of a by-value integer-like key as the hash opfamilies
 * compute it -- hashint2(), hashint4() (date too) and hashint8() (timestamp,
 * timestamptz) are these three lines -- for a probe that has the raw value
 * and would rather not call the function through fmgr. 'len' is the
 * column's attlen; the caller has checked that the operator's left-hand hash
 * function is the matching one of the three.
 */
static inline uint32
runtimefilter_hash_int(int64 v, int len)
{
	if (len == 8)
	{
		uint32		lohalf = (uint32) v;
		uint32		hihalf = (uint32) (v >> 32);

		lohalf ^= (v >= 0) ? hihalf : ~hihalf;
		return DatumGetUInt32(hash_uint32(lohalf));
	}
	return DatumGetUInt32(hash_uint32((int32) v));
}

/*
 * The probe watches the filter's usefulness: every RF_ADAPT_WINDOW rows,
 * once at least RF_ADAPT_MIN_ROWS have been tested, a filter that let more
 * than whpg_runtime_filter_disable_ratio of them through is switched off for
 * the rest of the query, and the rows go straight to the join: the planner's
 * estimate was wrong, or the keys are all present, and every probe from then
 * on would be a hash and a cache line for nothing. The tally is the
 * filter's own, so all who probe one filter in a process share it -- a
 * partitioned probe does not start over at every partition -- and a rebuilt
 * hash table's new filter starts afresh. The caller tests f->disabled first.
 */
#define RF_ADAPT_WINDOW			4096	/* a power of two */
#define RF_ADAPT_MIN_ROWS		65536

static inline void
runtimefilter_probe_count(RuntimeFilter *f, bool passed)
{
	f->seen++;
	if (passed)
		f->passed++;
	if ((f->seen & (RF_ADAPT_WINDOW - 1)) == 0 &&
		f->seen >= RF_ADAPT_MIN_ROWS &&
		(double) f->passed > whpg_runtime_filter_disable_ratio * (double) f->seen)
		f->disabled = 1;
}

/*
 * The clause the planner puts in the probe scan's qual: a call of
 * whpg_rf_match(filter, key, hashop, selectivity) whose first argument is
 * the exec Param the join sets (planrtfilter.c).
 */
extern Expr *runtimefilter_make_clause(int paramid, Expr *key, Oid hashop,
									   Oid collation, double selectivity);
extern bool runtimefilter_is_clause(const Node *node);
extern Node *runtimefilter_clause_key(const Node *clause);
extern void runtimefilter_split_qual(List *qual, List **filters, List **rest);

/*
 * Why the planner gave a hash join no filter (HashJoin.rf_decline), for
 * EXPLAIN under whpg_runtime_filter_explain. 0 means a filter was planned,
 * or the join was never considered.
 */
typedef enum RfDecline
{
	RF_DECLINE_NONE = 0,
	RF_DECLINE_JOIN_TYPE,		/* a join type that keeps unmatched probe rows */
	RF_DECLINE_NULL_MATCHING,	/* IS NOT DISTINCT FROM: NULL keys match */
	RF_DECLINE_SHAPE,			/* no Hash node on the inner side */
	RF_DECLINE_PARALLEL,		/* a parallel-aware join or scan */
	RF_DECLINE_PROBE_MOTION,	/* the probe rows come from another slice */
	RF_DECLINE_PROBE_NOT_SCAN,	/* no scan found, or the key is not its column */
	RF_DECLINE_KEY_STATS,		/* several keys, no statistics to choose one by */
	RF_DECLINE_PROBE_SMALL,		/* fewer probe rows than the gate */
	RF_DECLINE_UNSELECTIVE,		/* too many probe rows expected to match */
	RF_DECLINE_KEYS_UNSELECTIVE,	/* several keys, none selective enough alone */
	RF_DECLINE_BUILD_LARGE,		/* the inner side needs a filter over the size limit */
	RF_DECLINE_OP_NOT_HASHABLE,	/* the hash operator has no hash support function */
	RF_DECLINE_OP_NOT_STRICT,	/* a non-strict hash operator: NULL keys may match */
	RF_DECLINE_STAGED_SCAN		/* the clause would have cost the scan its Staged Scan */
} RfDecline;

extern const char *runtimefilter_decline_reason(int code);

#endif							/* RUNTIMEFILTER_H */
