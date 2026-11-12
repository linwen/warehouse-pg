/*-------------------------------------------------------------------------
 *
 * runtimefilter.c
 *	  Runtime filters for hash joins: the filter (a split-block Bloom filter
 *	  over join-key hash values), the probe of the clause the planner puts
 *	  in the probe scan's qual (a RuntimeFilterExpr), and the helpers that
 *	  recognize that clause.
 *
 * Once a hash join has built its hash table it knows exactly which join
 * keys the inner side holds. When the probe side's scan runs in the same
 * slice, the join hands that scan a Bloom filter over those keys through an
 * exec Param -- set by nodeHashjoin.c, read by the clause planrtfilter.c
 * adds to the scan's qual -- and the scan drops the rows that cannot have a
 * match before they are assembled, projected or handed up to the join. On an
 * AOCO table the clause can lead a Staged Scan, so the other columns are not
 * even decompressed for those rows.
 *
 * The filter is built from the same 32-bit hash values the hash table is
 * built with (ExecHashGetHashValue()), and the probe computes the value with
 * the hash function of the probe side's operand type -- the one the join
 * itself would have applied to the row. So a probe costs one hash function
 * call, which the join had to pay anyway, plus one cache line.
 *
 * The filter is a split-block Bloom filter: 256-bit blocks of eight 32-bit
 * words, one bit set in each word, so that a probe touches a single cache
 * line. It is sized from the planner's estimate of the inner rows at about
 * 16 bits per key (roughly 1% false positives) and capped by
 * whpg_runtime_filter_max_size; should the estimate have been far too low,
 * so that fewer than RF_MIN_BITS_PER_KEY bits per key remain, the finished
 * filter is dropped (the Param stays NULL) rather than let a dense filter
 * cost probes for nothing. A false positive only costs the hash table probe
 * the join would have made anyway; a NULL key is rejected, since a strict
 * hash operator never matches it.
 *
 * Portions Copyright (c) 2026, WarehousePG contributors.
 *
 * IDENTIFICATION
 *	    src/backend/utils/adt/runtimefilter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "nodes/execnodes.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_coerce.h"
#include "port/pg_bitutils.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/regproc.h"
#include "utils/runtimefilter.h"

/* GUCs */
bool		whpg_enable_runtime_filter = true;
int			whpg_runtime_filter_max_size = 4096;	/* kB */
double		whpg_runtime_filter_max_selectivity = 0.5;
int			whpg_runtime_filter_min_probe_rows = 10000;
double		whpg_runtime_filter_disable_ratio = 0.95;
bool		whpg_runtime_filter_explain = false;

#define RF_BITS_PER_KEY			16	/* sizing target */
#define RF_MIN_BITS_PER_KEY		6	/* planner gate: fewer and no filter is planned */
#define RF_MIN_LOG2_BLOCKS		8	/* 256 blocks, 8 kB */
#define RF_MAX_LOG2_BLOCKS		24	/* 512 MB, within MaxAllocSize */

/*
 * A finished filter with more than this fraction of its bits set is dropped:
 * with one bit per word in eight words, the false positive rate is about the
 * fill ratio to the eighth power, 17% here, and climbing fast beyond. Judged
 * on the bits actually set, not on the tuples inserted, since duplicate keys
 * set no new bits.
 */
#define RF_MAX_FILL				0.8

#define RF_HEADER_SIZE	offsetof(RuntimeFilter, blocks)
#define RF_SIZE(log2)	(RF_HEADER_SIZE + \
						 (((Size) 1 << (log2)) * RF_BLOCK_WORDS * sizeof(uint32)))

struct RuntimeFilterBuilder
{
	RuntimeFilter *filter;
	uint64		nkeys;
};

/* one odd multiplier per word of a block */
const uint32 runtimefilter_salt[RF_BLOCK_WORDS] = {
	0x47b6137bU, 0x44974d91U, 0x8824ad5bU, 0xa2b7289dU,
	0x705495c7U, 0x2df1424bU, 0x9efc4947U, 0x5c6bfb31U
};

static Size
rf_max_bits(void)
{
	return (Size) whpg_runtime_filter_max_size * 1024 * 8;
}

/* Can a filter over this many keys stay within the size limit and useful? */
bool
runtimefilter_fits(double nkeys)
{
	return nkeys * RF_MIN_BITS_PER_KEY <= (double) rf_max_bits();
}

/*
 * Start a filter sized for est_nkeys keys, allocated in cxt (the finished
 * filter stays there; the join frees it when it rebuilds or ends).
 */
RuntimeFilterBuilder *
runtimefilter_create(double est_nkeys, MemoryContext cxt)
{
	RuntimeFilterBuilder *b;
	RuntimeFilter *f;
	int			log2 = RF_MIN_LOG2_BLOCKS;
	double		want_bits = Max(est_nkeys, 1.0) * RF_BITS_PER_KEY;
	Size		max_bits = rf_max_bits();
	Size		size;

	while (log2 < RF_MAX_LOG2_BLOCKS &&
		   ((Size) 1 << log2) * RF_BLOCK_BITS < want_bits &&
		   ((Size) 1 << (log2 + 1)) * RF_BLOCK_BITS <= max_bits &&
		   RF_SIZE(log2 + 1) <= MaxAllocSize)
		log2++;

	size = RF_SIZE(log2);
	f = (RuntimeFilter *) MemoryContextAllocZero(cxt, size);
	SET_VARSIZE(f, size);
	f->magic = RF_MAGIC;
	f->log2_blocks = log2;

	b = (RuntimeFilterBuilder *) MemoryContextAlloc(cxt, sizeof(RuntimeFilterBuilder));
	b->filter = f;
	b->nkeys = 0;
	return b;
}

void
runtimefilter_insert(RuntimeFilterBuilder *b, uint32 hashvalue)
{
	uint64		h = runtimefilter_mix64(hashvalue);
	uint32	   *block = runtimefilter_block(b->filter, h);
	uint32		hm = (uint32) h;

	for (int i = 0; i < RF_BLOCK_WORDS; i++)
		block[i] |= 1U << ((hm * runtimefilter_salt[i]) >> 27);
	b->nkeys++;
}

/*
 * The finished filter, or NULL if it is too full to be worth probing (the
 * builder is freed either way). Fullness is measured on the bits set, so an
 * inner side of many rows over few distinct keys keeps its filter however
 * low the row estimate it was sized from.
 */
struct varlena *
runtimefilter_finish(RuntimeFilterBuilder *b)
{
	RuntimeFilter *f = b->filter;
	Size		nbits = ((Size) 1 << f->log2_blocks) * RF_BLOCK_BITS;
	uint64		set = pg_popcount((const char *) f->blocks, nbits / 8);

	if ((double) set > RF_MAX_FILL * (double) nbits)
	{
		pfree(f);
		f = NULL;
	}
	else
		f->nkeys = (uint32) Min(b->nkeys, PG_UINT32_MAX);
	pfree(b);
	return (struct varlena *) f;
}

/*
 * OR 'other' into 'into': the filter over the union of both key sets. Two
 * filters merge when they were sized alike, which filters built from the
 * same plan estimate are; the caller sizes them so. Returns false, leaving
 * 'into' alone, if they were not. This is what a filter spanning several
 * segments is made of, once a transport brings the parts together.
 */
bool
runtimefilter_merge(struct varlena *into, const struct varlena *other)
{
	RuntimeFilter *a = (RuntimeFilter *) into;
	const RuntimeFilter *b = (const RuntimeFilter *) other;
	Size		nwords;

	if (a->magic != RF_MAGIC || b->magic != RF_MAGIC ||
		a->log2_blocks != b->log2_blocks || a->flags != b->flags)
		return false;

	nwords = ((Size) 1 << a->log2_blocks) * RF_BLOCK_WORDS;
	for (Size i = 0; i < nwords; i++)
		a->blocks[i] |= b->blocks[i];
	a->nkeys = (uint32) Min((uint64) a->nkeys + b->nkeys, PG_UINT32_MAX);
	return true;
}

/*
 * The filter a Param holds, or NULL if what it holds is not a filter the
 * hash join built -- a guard against an executor or planner fault, since
 * only the planner writes the clause that reads it. NULL lets every row
 * through, as a Param the join has not set does: a filter only ever drops
 * rows the join would drop, so one that cannot be trusted is not an error,
 * it is no filter. The Staged Scan's fast path reads the Param the same way.
 */
struct varlena *
runtimefilter_check(struct varlena *v)
{
	RuntimeFilter *f = (RuntimeFilter *) v;

	if (VARSIZE(v) < RF_HEADER_SIZE ||
		f->magic != RF_MAGIC ||
		f->log2_blocks > RF_MAX_LOG2_BLOCKS ||
		VARSIZE(v) != RF_SIZE(f->log2_blocks))
	{
		elog(DEBUG1, "runtime filter ignored: the parameter does not hold a filter");
		return NULL;
	}
	return v;
}

/*
 * The probe of a RuntimeFilterExpr (primnodes.h), the clause the planner
 * puts in the probe scan's qual: true if the key may be in the filter. The
 * executor compiles the node to a step of its own (EEOP_RUNTIME_FILTER,
 * execExpr.c) whose state this is; the steps before it leave the key in the
 * state, and the clause rests on no catalog entry.
 *
 * A NULL filter -- none was built, or it was dropped -- accepts every row. A
 * NULL key is rejected when the hash operator is strict, since the join
 * never matches it then; a non-strict operator hashes it as 0, as
 * ExecHashGetHashValue() does (the planner gives such a join no filter; this
 * keeps the probe true to the join). A filter that turns out to pass nearly
 * every row is switched off for the rest of the query
 * (runtimefilter_probe_count() in runtimefilter.h). The key is hashed with
 * the hash operator's left-hand hash support function, exactly as
 * ExecHashTableCreate() hashes the probe side, looked up once per plan node.
 *
 * The probe cannot fail. The node is checked once per plan node, as a guard
 * against a planner or executor fault (only the planner writes one), and a
 * clause that fails a check lets every row through from then on: a filter
 * only ever drops rows the join would drop, so a clause that cannot be
 * trusted is no filter, not an error. The fault is logged, once per plan
 * node.
 */
static void
rf_probe_give_up(RuntimeFilterProbeState *probe, const char *why)
{
	probe->unusable = true;
	ereport(LOG,
			(errmsg("runtime filter ignored: %s", why)));
}

RuntimeFilterProbeState *
runtimefilter_probe_init(const RuntimeFilterExpr *rfexpr)
{
	RuntimeFilterProbeState *probe;
	Oid			keytype = exprType((Node *) rfexpr->key);
	Oid			lefttype;
	Oid			righttype;
	Oid			lhsproc;
	Oid			rhsproc;

	probe = (RuntimeFilterProbeState *) palloc0(sizeof(RuntimeFilterProbeState));
	probe->paramid = rfexpr->paramid;
	probe->collation = rfexpr->collation;

	/*
	 * The key must be what the operator's left-hand hash function expects,
	 * which is the operator's left-hand operand type (a date is hashed by
	 * hashint4, so the key's own type would be the wrong thing to compare
	 * with), and one the operator hashes (op_hashjoinable(): an array's
	 * elements need a hash function; the planner pass makes this same test,
	 * planrtfilter.c). Only a hash operator's own support function is ever
	 * called here.
	 */
	if (!OidIsValid(rfexpr->hashop))
		rf_probe_give_up(probe, "the clause names no hash operator");
	else if (!OidIsValid(keytype) ||
			 !op_hashjoinable(rfexpr->hashop, keytype) ||
			 !get_op_hash_functions(rfexpr->hashop, &lhsproc, &rhsproc))
		rf_probe_give_up(probe,
						 psprintf("operator %s is not hashable for a key of type %s",
								  format_operator(rfexpr->hashop),
								  OidIsValid(keytype) ?
								  format_type_be(keytype) : "unknown"));
	else
	{
		op_input_types(rfexpr->hashop, &lefttype, &righttype);
		if (!IsBinaryCoercible(keytype, lefttype))
			rf_probe_give_up(probe,
							 psprintf("operator %s does not take the key type %s",
									  format_operator(rfexpr->hashop),
									  format_type_be(keytype)));
		else
		{
			fmgr_info(lhsproc, &probe->hashfn);
			probe->opstrict = op_strict(rfexpr->hashop);
		}
	}
	return probe;
}

bool
runtimefilter_probe(RuntimeFilterProbeState *probe, ExprContext *econtext)
{
	ParamExecData *prm;
	RuntimeFilter *f;
	uint32		hashvalue;
	bool		result;

	/* a clause not to be trusted, or an ExprContext without the Params */
	if (probe->unusable || econtext->ecxt_param_exec_vals == NULL)
		return true;

	/* no filter (none built, or dropped) */
	prm = &econtext->ecxt_param_exec_vals[probe->paramid];
	if (prm->isnull)
		return true;

	/* a strict operator never matches a NULL key: the join drops the row */
	if (probe->keynull && probe->opstrict)
		return false;

	/* the pointer the hash join published */
	f = (RuntimeFilter *) runtimefilter_check((struct varlena *) DatumGetPointer(prm->value));
	if (f == NULL)
	{
		rf_probe_give_up(probe, "the parameter does not hold a filter");
		return true;
	}

	/* a filter that has shown itself useless lets the rows through */
	if (f->disabled)
		return true;

	/* a non-strict operator's NULL key hashes as 0, as in the join */
	if (probe->keynull)
		hashvalue = 0;
	else
		hashvalue = DatumGetUInt32(FunctionCall1Coll(&probe->hashfn,
													 probe->collation,
													 probe->keyvalue));
	result = runtimefilter_contains(f, hashvalue);
	runtimefilter_probe_count(f, result);

	return result;
}

/*
 * Build the clause: a RuntimeFilterExpr over the Param 'paramid', the key,
 * the join's hash operator ('hashop', with the key as its left-hand operand)
 * and the hash clause's input collation, which the hash function is called
 * with on both sides of the join.
 */
Expr *
runtimefilter_make_clause(int paramid, Expr *key, Oid hashop,
						  Oid collation, double selectivity)
{
	RuntimeFilterExpr *rfexpr = makeNode(RuntimeFilterExpr);

	rfexpr->paramid = paramid;
	rfexpr->key = key;
	rfexpr->hashop = hashop;
	rfexpr->collation = collation;
	rfexpr->selectivity = selectivity;
	return (Expr *) rfexpr;
}

/*
 * Is this the clause the planner builds? Only the planner writes a
 * RuntimeFilterExpr -- SQL has no syntax for one -- so a Staged Scan may
 * take the Param of a clause this accepts for one a hash join sets.
 */
bool
runtimefilter_is_clause(const Node *node)
{
	return node != NULL && IsA(node, RuntimeFilterExpr);
}

/* the key expression of a clause runtimefilter_is_clause() accepted */
Node *
runtimefilter_clause_key(const Node *clause)
{
	Assert(runtimefilter_is_clause(clause));
	return (Node *) ((const RuntimeFilterExpr *) clause)->key;
}

/* split a qual into its runtime filter clauses and the rest, order kept */
void
runtimefilter_split_qual(List *qual, List **filters, List **rest)
{
	ListCell   *lc;

	*filters = NIL;
	*rest = NIL;
	foreach(lc, qual)
	{
		Node	   *clause = (Node *) lfirst(lc);

		if (runtimefilter_is_clause(clause))
			*filters = lappend(*filters, clause);
		else
			*rest = lappend(*rest, clause);
	}
}

/* The reason behind a HashJoin's rf_decline, for EXPLAIN. */
const char *
runtimefilter_decline_reason(int code)
{
	switch ((RfDecline) code)
	{
		case RF_DECLINE_NONE:
			return NULL;
		case RF_DECLINE_JOIN_TYPE:
			return "the join keeps unmatched probe rows";
		case RF_DECLINE_NULL_MATCHING:
			return "NULL keys match (IS NOT DISTINCT FROM)";
		case RF_DECLINE_SHAPE:
			return "no Hash node on the inner side";
		case RF_DECLINE_PARALLEL:
			return "parallel-aware join or scan";
		case RF_DECLINE_PROBE_MOTION:
			return "the probe rows come from another slice";
		case RF_DECLINE_PROBE_NOT_SCAN:
			return "the probe side is not a scan in this slice, or the key is not a column of it";
		case RF_DECLINE_KEY_STATS:
			return "no statistics on the probe's key columns to choose a hash key by";
		case RF_DECLINE_PROBE_SMALL:
			return "fewer probe rows than whpg_runtime_filter_min_probe_rows";
		case RF_DECLINE_UNSELECTIVE:
			return "too many probe rows expected to match";
		case RF_DECLINE_KEYS_UNSELECTIVE:
			return "no single hash key is expected to drop enough probe rows";
		case RF_DECLINE_BUILD_LARGE:
			return "the inner side needs a filter over whpg_runtime_filter_max_size";
		case RF_DECLINE_OP_NOT_HASHABLE:
			return "the hash operator has no hash support function";
		case RF_DECLINE_OP_NOT_STRICT:
			return "a non-strict hash operator, whose NULL keys may match";
		case RF_DECLINE_STAGED_SCAN:
			return "the clause would have cost the scan its Staged Scan";
	}
	return "unknown";
}
