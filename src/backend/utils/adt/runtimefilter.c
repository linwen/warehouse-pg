/*-------------------------------------------------------------------------
 *
 * runtimefilter.c
 *	  Runtime filters for hash joins: the filter (a split-block Bloom filter
 *	  over join-key hash values), the probe function the planner puts in the
 *	  probe scan's qual, and the helpers that recognize that clause.
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

#include "catalog/pg_type.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "parser/parse_coerce.h"
#include "port/pg_bitutils.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
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
 * A Param that does not hold a filter the hash join built must be refused
 * before it is read -- a guard against an executor or planner fault, since
 * SQL cannot call the probe; the Staged Scan's fast path reads the Param the
 * same way and checks the same.
 */
struct varlena *
runtimefilter_check(struct varlena *v)
{
	RuntimeFilter *f = (RuntimeFilter *) v;

	if (VARSIZE(v) < RF_HEADER_SIZE ||
		f->magic != RF_MAGIC ||
		f->log2_blocks > RF_MAX_LOG2_BLOCKS ||
		VARSIZE(v) != RF_SIZE(f->log2_blocks))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid runtime filter")));
	return v;
}

/*
 * whpg_rf_match(filter internal, key anyelement, hashop oid, selectivity float8)
 *
 * Not callable from SQL: the filter argument is of type internal, which no
 * SQL expression yields, so the only caller is the clause the planner builds
 * (runtimefilter_make_clause()), its filter the Param a hash join sets.
 *
 * True if the key may be in the filter. A NULL filter -- none was built, or
 * it was dropped -- accepts every row. A NULL key is rejected when the hash
 * operator is strict, since the join never matches it then; a non-strict
 * operator hashes it as 0, as ExecHashGetHashValue() does (the planner gives
 * such a join no filter; this keeps the function true to the join). A filter
 * that turns out to pass nearly every row is switched off for the rest of the scan
 * (RuntimeFilterProbe in runtimefilter.h). 'hashop' is the
 * join's hash operator, whose left-hand operand is the key: the key is hashed
 * with that operator's left-hand hash support function, exactly as
 * ExecHashTableCreate() hashes the probe side, looked up once per plan node.
 * The selectivity argument is read only by the planner, through the support
 * function below.
 */
typedef struct RfMatchCache
{
	FmgrInfo	hashfn;
	bool		opStrict;		/* does the hash operator reject NULL? */
	RuntimeFilterProbe probe;	/* the adaptive switch-off, per filter */
} RfMatchCache;

Datum
whpg_rf_match(PG_FUNCTION_ARGS)
{
	RfMatchCache *cache = (RfMatchCache *) fcinfo->flinfo->fn_extra;
	RuntimeFilter *f;
	uint32		hashvalue;
	bool		result;

	if (PG_ARGISNULL(0))
		PG_RETURN_BOOL(true);

	if (cache == NULL)
	{
		Oid			keytype = get_fn_expr_argtype(fcinfo->flinfo, 1);
		Oid			hashop;
		Oid			lefttype;
		Oid			righttype;
		Oid			lhsproc;
		Oid			rhsproc;

		/*
		 * The operator is looked up once per plan node, so it has to be the
		 * same on every row: the planner passes a Const. These checks, and
		 * the ones below, guard against a planner fault; no caller but the
		 * planner's clause reaches them.
		 */
		if (PG_ARGISNULL(2) || !get_fn_expr_arg_stable(fcinfo->flinfo, 2))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("runtime filter: the hash operator must be a constant")));
		hashop = PG_GETARG_OID(2);

		/*
		 * The key must be what the operator's left-hand hash function
		 * expects, which is the operator's left-hand operand type (a date is
		 * hashed by hashint4, so the function's own argument type would be
		 * the wrong thing to compare with). The planner always pairs them
		 * so. Only a hash operator's own support function is ever called
		 * here. The function is not marked leakproof: it refuses a
		 * malformed filter with an error, which a leakproof function must
		 * not do, and nothing needs the mark -- the clause the planner
		 * makes cannot fail, its filter being a Param the join sets, and the
		 * Staged Scan knows it by that Param (stagedscan_clause_can_fail()).
		 */
		if (!OidIsValid(keytype) || !op_hashjoinable(hashop, keytype) ||
			!get_op_hash_functions(hashop, &lhsproc, &rhsproc))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("runtime filter: operator %s is not hashable",
							format_operator(hashop))));
		op_input_types(hashop, &lefttype, &righttype);
		if (!IsBinaryCoercible(keytype, lefttype))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("runtime filter: operator %s does not take the key type %s",
							format_operator(hashop), format_type_be(keytype))));

		cache = (RfMatchCache *)
			MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt, sizeof(RfMatchCache));
		fmgr_info_cxt(lhsproc, &cache->hashfn, fcinfo->flinfo->fn_mcxt);
		cache->opStrict = op_strict(hashop);
		fcinfo->flinfo->fn_extra = cache;
	}

	/* a strict operator never matches a NULL key: the join drops the row */
	if (PG_ARGISNULL(1) && cache->opStrict)
		PG_RETURN_BOOL(false);

	/* an internal-typed argument: the pointer the hash join published */
	f = (RuntimeFilter *) runtimefilter_check((struct varlena *) PG_GETARG_POINTER(0));

	/* a rebuilt hash table brings a new filter: judge it afresh */
	runtimefilter_probe_set(&cache->probe, f);
	if (cache->probe.disabled)
		PG_RETURN_BOOL(true);

	/* a non-strict operator's NULL key hashes as 0, as in the join */
	if (PG_ARGISNULL(1))
		hashvalue = 0;
	else
		hashvalue = DatumGetUInt32(FunctionCall1Coll(&cache->hashfn,
													 PG_GET_COLLATION(),
													 PG_GETARG_DATUM(1)));
	result = runtimefilter_contains(f, hashvalue);
	runtimefilter_probe_count(&cache->probe, result);

	PG_RETURN_BOOL(result);
}

/*
 * Planner support for whpg_rf_match(): its selectivity is the planner's
 * estimate of the probe rows that have a match, carried in the fourth
 * argument so that the cost model (the Staged Scan's in particular) can
 * weigh the clause like any other.
 */
Datum
whpg_rf_match_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	if (IsA(rawreq, SupportRequestSelectivity))
	{
		SupportRequestSelectivity *req = (SupportRequestSelectivity *) rawreq;

		if (list_length(req->args) == 4)
		{
			Node	   *selarg = (Node *) lfourth(req->args);

			if (IsA(selarg, Const) && !((Const *) selarg)->constisnull)
			{
				double		sel = DatumGetFloat8(((Const *) selarg)->constvalue);

				req->selectivity = Max(0.0, Min(1.0, sel));
				PG_RETURN_POINTER(req);
			}
		}
	}
	PG_RETURN_POINTER(NULL);
}

/*
 * Build the clause: whpg_rf_match($paramid, key, hashop, selectivity).
 * 'hashop' is the join's hash operator, with the key as its left-hand
 * operand; 'collation' is the hash clause's input collation, which the hash
 * function is called with on both sides of the join.
 */
Expr *
runtimefilter_make_clause(int paramid, Expr *key, Oid hashop,
						  Oid collation, double selectivity)
{
	Param	   *param = makeNode(Param);
	Const	   *opConst;
	Const	   *selConst;

	param->paramkind = PARAM_EXEC;
	param->paramid = paramid;
	param->paramtype = INTERNALOID;
	param->paramtypmod = -1;
	param->paramcollid = InvalidOid;
	param->location = -1;

	opConst = makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
						ObjectIdGetDatum(hashop), false, true);
	selConst = makeConst(FLOAT8OID, -1, InvalidOid, sizeof(float8),
						 Float8GetDatum(selectivity), false, FLOAT8PASSBYVAL);

	return (Expr *) makeFuncExpr(F_WHPG_RF_MATCH, BOOLOID,
								 list_make4(param, key, opConst, selConst),
								 InvalidOid, collation, COERCE_EXPLICIT_CALL);
}

/*
 * Is this the clause the planner builds? A call of whpg_rf_match() whose
 * filter argument is an exec Param of type internal. Only the planner can
 * write such a call: no SQL expression yields a value of type internal -- a
 * scalar subquery's Param, a prepared statement's $1, a column are all of
 * SQL types -- so the parser never resolves whpg_rf_match() for a query, and
 * a Staged Scan may take the Param of a clause this accepts for one a hash
 * join sets.
 */
bool
runtimefilter_is_clause(const Node *node)
{
	const FuncExpr *fe;
	const Node *filter;

	if (node == NULL || !IsA(node, FuncExpr))
		return false;
	fe = (const FuncExpr *) node;
	if (fe->funcid != F_WHPG_RF_MATCH || list_length(fe->args) != 4)
		return false;
	filter = (const Node *) linitial(fe->args);
	return IsA(filter, Param) &&
		((const Param *) filter)->paramkind == PARAM_EXEC &&
		((const Param *) filter)->paramtype == INTERNALOID;
}

/* the key expression of a clause runtimefilter_is_clause() accepted */
Node *
runtimefilter_clause_key(const Node *clause)
{
	Assert(runtimefilter_is_clause(clause));
	return (Node *) lsecond(((const FuncExpr *) clause)->args);
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
