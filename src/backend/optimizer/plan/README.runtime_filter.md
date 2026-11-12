# Runtime filters for hash joins

A hash join knows, once its hash table is built, exactly which join keys the
inner side holds. A runtime filter hands that knowledge to the probe side's
scan, so that rows whose key cannot be in the table are dropped where they are
read, before they are assembled, projected, or handed up to the join. The
idea is as old as Bloomjoin (Mackert & Lohman, 1986) and is in every MPP
engine under some name -- bitmap filter, join filter, dynamic filter,
sideways information passing; this is WarehousePG's implementation of the
*local* form: join and probe scan in the same slice, no network in between.

Code:

| piece | file |
|---|---|
| the filter: a split-block Bloom filter in a bytea, build/insert/finish/probe | `src/backend/utils/adt/runtimefilter.c` |
| the probe: `RuntimeFilterExpr` (primnodes.h), the clause the planner puts in the scan's qual, which the executor compiles to a step of its own (`EEOP_RUNTIME_FILTER`); no catalog entry | same file; `execExpr.c`, `execExprInterp.c` |
| the planner pass: which joins, which scan, the Param, the clause | `src/backend/optimizer/plan/planrtfilter.c` |
| hooks: after `SS_finalize_plan()` for the Postgres planner, after the Staged Scan pass for GPORCA | `planner.c` (`standard_planner`), `orca.c` (`optimize_query`) |
| building the filter while the table is built | `nodeHash.c` (`MultiExecPrivateHash`) |
| publishing it through the Param, before the first probe row | `nodeHashjoin.c` (`ExecHashJoin*RuntimeFilter`) |
| the Staged Scan reading the Param inside the access method | `nodeSeqscan.c`, `aocsam.c`, `aocs_stagedscan.c`, `stagedscancost.c` |
| plan fields | `HashJoin.rf_paramids` in `plannodes.h`; the clause itself is in `SeqScan.scan.plan.qual` |
| EXPLAIN | `show_runtime_filters()` in `explain.c` |
| GUCs | `guc_gp.c`, listed in `sync_guc_name.h` |
| test | `src/test/regress/sql/runtime_filter.sql` |

## How it runs

**Planning.** After either optimizer has finished its plan, a pass walks it
(`runtimefilter_decide_plan()`). For every hash join it looks for the scan the
probe rows come from: the join's outer input, through Result nodes and down
the outer side of inner, left, semi and anti joins -- a star join is a chain
of hash joins over one fact scan, and every dimension's filter can be tested
there -- stopping at a Motion, a Material, a Sort, a right or full join, an
Append or anything else. If that is a SeqScan or DynamicSeqScan of a base
relation and the join's outer hash key is a column of it, carried down the
outer side unchanged, the pass

- allocates an exec Param (`paramExecTypes`, type internal),
- puts a `RuntimeFilterExpr` over that Param, the column, the join's hash
  operator and the survivor estimate after the scan's own clauses -- they
  are cheaper and already in cost order, so the filter sees only the rows
  they pass,
- records the Param on the join (`rf_paramids`),
- and, for an AOCO table, decides the Staged Scan again with the new clause
  in the predicate (`stagedscan_decide_plan_scan()`), so that the join key
  can lead Phase A. A clause that would cost a staged scan its Staged Scan
  is left out again: the Staged Scan model re-costs one filter column as if
  it were clustered (a single-stage Phase A can fall back when the
  survivors turn out dense) but takes a multi-column predicate at its
  pessimistic estimate, and the staged read of the first key is worth more
  than a second clause in the ordinary path. The joins are visited
  bottom-up, so the lowest join's filter is the one a scan keeps.

The clause's selectivity -- the join's estimated output rows over the probe's
rows, exact when a probe row matches at most one inner row and an
overestimate of the survivors otherwise -- travels in the clause's fourth
argument and is returned by the function's planner support, so
`clause_selectivity()` and the Staged Scan cost model treat the clause like
any other. The hash function is the operator's left-hand one, the same
`ExecHashTableCreate()` applies to the probe side.

**One key of several.** A join on several keys (TPC-DS joins sales to
returns on `(ticket_number, item_sk)`) gets a filter over one of them. A
probe row passes a filter over one key when that key alone has a match,
whatever the others do, so the pass picks the key expected to let the fewest
rows through, estimated per key from the statistics
(`rf_key_match_fraction()`): the inner side's distinct values of the key --
its rows, capped by the column's distinct values when the key is a column
of some relation (`rf_trace_column()` follows it down the inner plan) --
over the probe column's distinct values, the inner side's keys being taken
for a subset of the probe's, as a foreign key's are. For the returns join
`item_sk` covers every item and `ticket_number` a few thousand of millions,
so the filter is over `ticket_number`. Rows in a plan are a segment's; a
segment's inner rows cover the same share of the probe column whether they
are the whole inner side (a Broadcast) or its hash range, except when that
range is the probe table's own distribution on the key -- then the segment's
probe rows are that range too, and the inner side's rows over all segments
count. The share is never put below the join's own estimate, and the usual
selectivity gate applies to it; a join whose probe column has no statistics
gets no filter. The Hash node hashes the chosen key by itself into the
filter (`HashJoin.rf_keyidx`; the hash value it computes for the table
combines all keys), one more evaluation of that key per inner row, and the
clause hashes the probe column the same way.

**Executing.** The join initializes its Param to NULL before its children are
initialized (`ExecInitHashJoin`). When it builds the hash table it hands the
Hash node a builder sized from the planner's estimate of the inner rows; the
Hash node inserts every tuple's hash value -- the one it computes anyway for
the bucket -- and the join then points the Param at the finished filter. Only
then does it read the first probe row: a join with a filter never fetches an
outer row first to check for an empty outer. On a rebuild (a rescan with the
inner's parameters changed) the Param goes back to NULL and a new filter
takes its place.

The probe side's scan evaluates the clause per row like any qual. On a heap
or AO row table that is `ExecQual()` after the row is fetched: it saves the
hash table probe, with its cache misses, for every row the filter rejects.
On an AOCO table the clause is part of the Staged Scan's predicate, so Phase
A reads the join key column alone and the rows it drops never have their
other columns decompressed; that is where most of the time goes. The AOCO
access method evaluates its stage quals in standalone ExprContexts, which
carry no Param values; the scan node hands it the executor's
(`AOCSScanDescData.stagedParamExecVals`), and the applicability checks that
decline quals with Params make an exception for this clause.

When the key column is of a fixed-width integer-like type (int2/int4/int8,
date, timestamp, timestamptz) the clause also joins the Staged Scan's raw-datum
fast path (`stagedscan_compile_simple()`): a NULL-free block in the original
datum layout is tested value by value off its datum array, each value hashed
-- inline for the integer hash functions, through the operator's own hash
support function otherwise -- and looked up in the filter, with no tuple
formed and no `ExecQual()`. The filter is bound from the Param once per block
(`stagedscan_rf_bind()`), and the switch-off below counts those rows too. A
block the fast path cannot take (NULLs, RLE/delta layout) goes row by row
through the qual's probe as before.

The probe returns true for a NULL filter (not built yet, dropped as
too full, a Parallel Hash) and false for a NULL key: a strict hash operator
never matches a NULL, and the join types that get a filter drop such rows.
A join over a non-strict hash operator, which hashes NULL as 0 and may match
it (`ExecHashGetHashValue()`), gets no filter.
It also watches the filter's usefulness: every 4096 rows, once 65536 have
been tested, a filter that let more than `whpg_runtime_filter_disable_ratio`
of them through is switched off for the rest of the query, and the rows go
straight to the join -- the estimate was wrong, or the keys are all there,
and every further probe would be a hash and a cache line for nothing. The
tally is kept in the filter itself (`RuntimeFilter.seen`, `.passed`,
`.disabled`), so every probe of one filter in a process -- the qual's, the
Staged Scan fast path's, each partition's scan under a dynamic scan -- counts
into the same tally and gives it up together, rather than each starting the
65536 rows over; a rebuilt hash table brings a new filter, judged afresh.
The clause is a node of its own, `RuntimeFilterExpr` (primnodes.h), which
only the planner writes: SQL has no syntax for it, and it rests on no
catalog entry -- no `pg_proc` row, no catalog version change -- so a minor
release can carry it onto an existing cluster. The executor compiles it to
one step (`EEOP_RUNTIME_FILTER`, `execExpr.c`) that evaluates the key and
probes the filter without going through fmgr; the JIT calls the same C
function (`ExecEvalRuntimeFilter()`). The step's setup
(`runtimefilter_probe_init()`) checks, once per plan node, that the hash
operator takes the key's type and hashes it (an array key needs a hash
function for its elements, `op_hashjoinable()`), and the probe checks that
the Param holds a filter, as guards against a planner or executor fault --
and a clause that fails a check lets every row through from then on, with a
LOG line, rather than fail the query: a filter only ever drops rows the join
would drop, so an untrusted one is no filter, not an error. The planner pass
makes the same tests before it writes a clause, so the guards should never
fire.

**The filter** is a split-block Bloom filter: 256-bit blocks of eight 32-bit
words, one bit per word, so a probe touches a single cache line; it is fed
the hash table's own 32-bit hash values, spread to 64 bits by a mixing
function, block from the top half, bits from the bottom. It is sized at 16
bits per expected key (about 1% false positives), never below 8 kB and never
above `whpg_runtime_filter_max_size`. Should it come out too full -- more
than 80% of its bits set once the table is built, about 17% false positives
and climbing fast beyond -- the filter is dropped rather than probed for
nothing; fullness is judged on the bits set, not on the tuples inserted, so an
inner side of many rows over few keys keeps its filter however low the row
estimate it was sized from. A false positive costs the hash table probe the
join would have made anyway.

## What is guaranteed

The join's answer is unchanged. The filter can only reject a probe row whose
key is not in the hash table (Bloom filters have no false negatives, and the
probe uses the same hash function and value as the table), and for the join
types that get a filter such a row never contributes to the output: INNER,
SEMI, RIGHT (the hash side is the preserved one), and the dedup-semi
variants. LEFT, FULL, ANTI and NOT-IN joins keep the rows the filter would
drop, and get none. An IS NOT DISTINCT FROM join (`hashqualclauses`) matches
NULLs, and gets none. A row with a NULL key is rejected; a strict hash
operator never matches it, and a join over a non-strict one gets none. The
Staged Scan's rules carry over: the clause cannot fail -- its filter is a
Param the join sets to NULL or to a filter it built, its operator and key
type paired by the planner -- so it may be decided at any stage, ahead of
clauses that can fail; `stagedscan_clause_can_fail()` knows it by its node
type, which only the planner writes (`runtimefilter_is_clause()`).

Passing a lower join on the way to the scan is safe for the same reason. A
probe row the filter drops before an inner, left, semi or anti join (the
fact on the outer side) takes only its own descendants with it: every row the
join derives from it carries the key column unchanged, so the upper join
would have dropped them all. A RIGHT or FULL join is not passed: an inner
row matched by nothing else would then be emitted null-extended in the
dropped row's place, a row the plan without the filter never produced.
Neither are the dedup-semi variants, which choose between duplicates by a
row identity a dropped row could alter. A key taken from a lower join's
inner side stops the search.

Nothing about the plan's shape changes: the same nodes, the same Motions, one
more clause on one scan per filter. A plan without a filter runs exactly as
before; a plan with one whose filter is NULL runs as before plus one cheap
call per row.

## What is declined

- a probe under a Motion (the filter would have to cross slices; see below),
  under an Append, a right or full join, a dedup-semi join, a Sort or
  anything but a Result or an inner, left, semi or anti join whose outer side
  carries the key;
- a probe under a Material: a Material rescanned with its child unchanged
  replays what it stored, so rows the previous filter dropped would never be
  read again after the hash table is rebuilt with other keys (the inner side
  parameterized by an outer loop); the rows a filter drops must come from a
  scan that is re-read on every rescan;
- a join key that is not a plain column of the scan (`k + 1`, a function);
- a join on several keys when no key is such a column under a hashable,
  strict operator, when the probe's key columns have no statistics to choose
  one by, or when no key alone is expected to drop enough probe rows (see
  "One key of several" below);
- parallel-aware joins or scans (the Param would have to reach the workers);
- a subquery's plan that still has its own range table (Postgres planner
  before `set_plan_references()`: a `SubqueryScan`'s plan and the `TABLE()`
  argument of a table function are not entered; GPORCA's range table is
  flat and its subplans are walked);
- plans under row-level security, as for the Staged Scan;
- a probe estimated below `whpg_runtime_filter_min_probe_rows`, a join
  expected to keep more than `whpg_runtime_filter_max_selectivity` of the
  probe rows, or an inner side that would need a filter above
  `whpg_runtime_filter_max_size` to stay useful.

## GUCs

| GUC | default | meaning |
|---|---|---|
| `whpg_enable_runtime_filter` | on | master switch, both optimizers |
| `whpg_runtime_filter_max_size` | 4 MB | the largest filter planned (64 kB to 64 MB; one per join and segment process, outside the join's operator memory); a denser one is dropped at run time |
| `whpg_runtime_filter_max_selectivity` | 0.5 | developer: no filter when more of the probe is expected to match |
| `whpg_runtime_filter_min_probe_rows` | 10000 | developer: no filter for a smaller probe (rows as the plan node estimates them) |
| `whpg_runtime_filter_disable_ratio` | 0.95 | developer: a filter letting more of the probe rows through than this is switched off by the scan; 1 never switches off |
| `whpg_runtime_filter_explain` | off | developer: EXPLAIN adds `Runtime Filter: declined: <reason>` under every hash join the planner looked at and gave no filter (join type, probe under a Motion, probe not a scan, too small, too many matches, no key of several selective enough, ...) |

The first five are synchronized to the segments; the last is read by EXPLAIN
on the coordinator.

EXPLAIN shows the clause apart from the predicate, by the column it filters:

```
->  Staged Seq Scan on rf_fact_aoco f
      Runtime Filter: k
      Stages: k
```

`Rows Removed by Filter` counts the rows the filter and the predicate removed
together, whichever did it.

## Measured

Development cluster, 3 primaries on one machine, cassert build, warm cache,
GPORCA. A 6M-row fact of 21 columns (`bf_fact`: 19 integers, one md5 text,
AOCO zstd) joined to a 100k-key dimension of which 1% passes the filter, so
1% of the fact rows have a match; the join keys are spread uniformly over the
fact. Four columns aggregated. Median of two warm runs.

| probe table | filter off | filter on | plan with the filter |
|---|---|---|---|
| heap copy of the fact | 210-239 ms | 107 ms | `Seq Scan ... Runtime Filter: k` |
| AOCO zstd | 270-330 ms | 212-218 ms | `Staged Seq Scan ... Runtime Filter: k`, every segment file fell back |

On the heap table the saving is the hash table probe (hash, bucket, compare)
for 99% of the rows. On the AOCO table the planner made the scan a Staged Scan
with the filter leading Phase A, but the first-batch density check handed
every segment file back to the ordinary scan: with the keys spread uniformly,
every 8000-row varblock of the key column holds some 80 matches, so Phase B
could skip no block and, since an AOCO varblock is read forward datum by
datum, would advance through nearly every datum of the other columns anyway
(`stagedscan_survivors_too_dense()`). The filter then ran as the first clause
of the ordinary scan, and the gain is again the probe. The same table with a
*clustered* 1% predicate keeps the Staged Scan (`Stage Survivors: 60000 of
6000000 rows`), and so would a filter whose matches cluster in insertion
order -- a dimension filtered to recent dates against a fact loaded by date,
say -- or are sparse enough to leave most blocks empty.

The 200k-row test table (`runtime_filter.sql`) and the 50k-row smoke test keep
the Staged Scan with the filter because their segment files are shorter than
the 64 blocks the density check needs before it decides; EXPLAIN ANALYZE
there shows Phase A keeping 2-8% of the rows.

**A useless filter**, the same heap fact against the whole 100k-key
dimension (every probe row has a match) with the selectivity gate lifted so
that a filter is planned anyway: 345 ms with no filter; 658 / 629 ms with the
filter and the switch-off disabled (`whpg_runtime_filter_disable_ratio = 1`);
403 / 380 ms with the switch-off at its default -- the scan gives the filter
up after its first 65536 rows, and what remains is that window plus the call
the qual still makes per row. (Without the switch, a wrong estimate would
cost almost as much as a right one gains.)

**TPC-DS, scale factor 10**, the same cluster, the repository's AOCO/zstd
schema (`store_sales` range-partitioned by `ss_sold_date_sk` into 221
partitions), GPORCA, warm cache, two runs per setting; ten star-join queries
with selective dimension predicates:

| query | filters planned | filter off (ms) | filter on (ms) |
|---|---|---|---|
| q03 | 1 (`ss_item_sk`) | 407 / 399 | 310 / 299 |
| q07 | 1 (`ss_cdemo_sk`) | 755 / 679 | 540 / 536 |
| q27 | 1 (`ss_cdemo_sk`) | 803 / 810 | 646 / 640 |
| q55 | 1 (`ss_item_sk`) | 258 | 234 / 232 |
| q73 | 2 (`ss_hdemo_sk`, `c_customer_sk`) | 520 | 486 / 495 |
| q52 | 1 (`ss_item_sk`) | 235 / 260 | 226 / 236 |
| q19 | 2 (`ca_address_sk`, `c_customer_sk`) | 326 / 333 | 321 / 318 |
| q42 | 1 (`ss_item_sk`) | 239 / 233 | 242 / 233 |
| q43, q98 | 0 | unchanged | unchanged |

Here the filter does lead the Staged Scan on the fact table and stays: each
partition's segment file is far shorter than the 64 blocks the density check
needs, so Phase A runs to the end. q03 (`i_manufact_id = 888`, 24 items):
`Dynamic Staged Seq Scan on store_sales ... Runtime Filter: ss_item_sk, Stage
Survivors: 1579 of 6988326 rows, Stage Blocks: 1482 decompressed, 748
skipped` -- 99.98% of the rows dropped on the key column alone, a third of the
other columns' blocks never decompressed. q07 (`customer_demographics`
filtered to 1.4%): `Stage Survivors: 81355 of 5773374 rows`, 6224 blocks
decompressed, 140 skipped. The whole query gains 20-25% because the scan is
followed by the date join, Motions and the aggregate, which the filter does
not touch. At this scale every hash table fits in cache, so the probe the
filter saves is cheap; the relative gain grows with the scale.

All 99 queries, each run once with the filter off and once on (filter off
first, so the on run has the warmer cache), on the final code: the planner
gave 57 of them a filter; the whole set took 292.8 s off and 267.2 s on
(-8.7%), the 57 with a filter 108.1 s and 83.5 s (-22.8%); but one cold off
run (q07, 17.1 s against its usual 0.7 s) inflates both totals, and without
it they read -3% and -9%. The robust figures are the median change per
query, -6.2%, and the counts: 31 queries more than 5% faster, 3 more than 5%
slower (q72 +10% at 22-24 s, q89 +9%, q93 +5%), all three within what the
queries *without* a filter -- identical plans -- showed between their two
runs (-32% to +15%). An earlier sweep, before the clause was moved behind
the scan's own quals and before the switch-off, had a median of -3.7% and
10 queries more than 5% slower; the same 57 queries got a filter both times.
Every query returned the same rows on and off; four (q64, q65, q71, q75)
listed them in another order, two of those with no filter in the plan: tie
order under `ORDER BY ... LIMIT`.

**TPC-DS, scale factor 30**, release build (no assertions) with the Phase A
fast path and the filters carried down join chains, GPORCA, the same schema
(`store_sales` 86M rows in 221 partitions, an 11 GB database) and cluster.
Each query ran four times in the order on, off, on, off and the second pair
is compared: the first run of every query was also the first touch of its
data after the load, so the first pair carries the cold read on the "on"
side (q07's first on run took 5.9 s against 0.6 s warm). A query whose two
off runs disagreed by more than a quarter, or whose warm on run came out
slower than both off runs by that much, was run again alone and the second
measurement kept (q11, q16, q18, q69, q81; q56, q88, q90 and q96 were
re-timed after the rule that keeps a scan's Staged Scan ahead of a second
filter). q23 and q95, the two largest spillers, were left out: q95 runs
out of temporary space on this volume under both settings, q23 takes 100 s
and gets no filter. 61 of the 97 queries got a filter. Warm pair: the 97
queries 344.6 s off, 327.7 s on (-4.9%); the 61 with a filter 103.1 s and
84.6 s (-17.9%); the 36 without one -- identical plans -- 241.5 s and
243.1 s (+0.6%). Per query with a filter: median -7.6%, 40 queries more
than 5% faster, 4 more than 5% slower; without a filter the median change
is +0.2% and the spread -9%..+15%. Taking the minimum of the two runs per
setting instead: -4.6% / -16.4%, median -7.0%, 38 faster and 4 slower.
Before the filters were carried down join chains the same sweep had 55
queries with a filter, at -9.8% for them and a median of -7.2%; the six
queries that gained a filter and the eleven that gained more account for
the difference (q16 -40% to -58%, q48 -8% to -40%, q19 -9% to -32%, q66 0
to -30%, q72 -6% to -18%).

| query | filters | filter off (ms) | filter on (ms) | change |
|---|---|---|---|---|
| q16 | 3 | 3201 | 1350 | -58% |
| q03 | `ss_item_sk` | 489 | 221 | -55% |
| q94 | 3 | 1761 | 878 | -50% |
| q63 | `ss_item_sk` | 519 | 269 | -48% |
| q90 | 2 | 922 | 534 | -42% |
| q84 | 3 | 230 | 134 | -42% |
| q48 | 2 | 1231 | 737 | -40% |
| q53 | `ss_item_sk` | 602 | 363 | -40% |
| q96 | `ss_sold_time_sk` | 2130 | 1347 | -37% |
| q07 | `ss_cdemo_sk` | 877 | 583 | -34% |
| q88 | 8 | 12704 | 8358 | -34% |
| q19 | 1 | 244 | 165 | -32% |
| q26 | 1 | 507 | 346 | -32% |
| q55 | `ss_item_sk` | 146 | 100 | -32% |
| q27 | `ss_cdemo_sk` | 998 | 693 | -31% |
| q66 | 4 | 1069 | 750 | -30% |
| q72 | 2 | 13965 | 11440 | -18% |

q03 shows what the fast path changes: `Dynamic Staged Seq Scan on
store_sales ... Runtime Filter: ss_item_sk, Rows Removed by Filter: 6977374,
Stage Survivors: 17969 of 17066395 rows, Stage Blocks: 5000 decompressed, 244
skipped`. The 24 matching items leave a survivor in nearly every block, so
Phase B skips almost nothing, and the gain is Phase A's alone: 17M key
values hashed and probed off the datum array, 18k rows assembled. At scale
factor 10, with the clause evaluated row by row, the same query gained 25%.

The slower ones are small and real: q21 (132 to 147 ms), q89 (750 to 819
ms, both runs), q93 (67 to 72 ms) and q83 (297 to 314 ms, nine filters over
small scans). q21 and q89 are alike: on q89, `Rows Removed by Filter:
4910572` of 5.5M rows and `Stage Survivors: 0 of 0` -- the 3005 matching
items (7.5% of `item`) put survivors in every block, the density check
handed every partition back to the ordinary scan at once, and the clause
ran as a per-row function call to save 4.9M probes of a 3005-row hash table
that sits in L2 cache, a probe no filter can beat. The switch-off does not
fire: the filter rejects 89% of the rows, useful by its own measure and
useless by the clock. Every query returned the same rows on and off; q64,
under `ORDER BY ... LIMIT 100`, listed different tied rows between two runs
of the *same* setting in an earlier sweep (without the LIMIT its 582 result
rows are identical on and off).

**The fast path alone.** A 12M-row AOCO fact of 8 columns joined to a
120k-key dimension, 1% of the fact's keys, clustered at the start of each
segment file so that Phase A runs to the end (`Stage Survivors: 121577 of
12000000 rows, Stage Blocks: 2437 decompressed, 3041 skipped`); the same join
once on the key column, whose blocks are NULL-free and take the fast path,
and once on a copy of it with one NULL per 4000 rows, which sends every block
down the row-by-row path. Five rounds each, release build, same binary:

| key column | filter off | filter on |
|---|---|---|
| NULL-free (fast path) | 285-310 ms | 89-92 ms |
| one NULL per block (row by row) | 357-384 ms | 250-259 ms |

The NULL-bearing copy costs the ordinary scan some 75 ms more on its own, so
the clause's own cost over 12M rows comes to about 90 ms on the fast path
against about 180 ms row by row; the filtered scan is 2.8x faster with the
fast path than without it, and 3.3x faster than the unfiltered scan.

Two queries (q58, q83) first failed on the probe function's own type check,
which compared the key type with the hash function's declared argument type:
a `date` key is hashed by `hashint4`, by the hash opfamily's design. The
clause now carries the hash operator, and the check is against the operator's
left-hand operand type, as `ExecHashTableCreate()` pairs them.

## Toward cross-segment filters

The form implemented here is the first phase: the fuller feature lets a
filter also reach a probe scan on the far side of a Redistribute Motion, so
that the rows are dropped before they are sent, saving the network as well as
the probe. That design weighs two ways of getting the filter across -- rewriting
the plan so that the filter is aggregated and broadcast as a tuple that a
NestLoop injects as a Param (plan-level, like Spark and Flink), or a
downstream channel in the interconnect from the join's slice to the scan's
(executor-level, like Impala and Doris) -- and in either the scan side is
the same. What is here is built to be that scan side:

- **The hand-off is an exec Param holding a bytea.** Whatever produces the
  filter -- this join, a NestLoop's `nestParams` from a broadcast tuple, or a
  receiver in the scan's slice -- sets `es_param_exec_vals[p]` and the scan
  reads it through the same clause. The clause reads the Param on every row,
  so a filter that arrives after the scan has started takes effect from the
  next row, and a NULL Param (not arrived, dropped, timed out) means "no
  filter". The Staged Scan's Param plumbing works for any producer.
- **The filter format is mergeable and self-describing.** The bytea carries
  a magic, the block count and the key count; two filters of the same block
  count merge by OR (`runtimefilter_merge()`), which is what a segment-wide
  filter is: the OR of per-segment filters over the same hash values. Block
  counts agree when every builder sizes from the same plan estimate, as they
  do. Nothing in it is specific to one process.
- **The hash values are the hash table's.** They are the opfamily's hash
  functions, deterministic across segments, so a filter built on one segment
  is valid for probes on another; a partitioned filter (one per destination
  segment of the probe's Motion) would be built from the same values.
- **The planner pass is a post-processing pass over the finished plan**, for
  both optimizers, which is where the plan-level design wants its rewrite as
  well. `rf_find_probe_scan()` reports a Motion as its own outcome
  (`RF_PROBE_MOTION`), distinct from the shapes that get no filter; that
  branch in `rf_consider_hashjoin()` is where the cross-segment version
  chooses its transport and continues to the scan on the far side.
- **`HashJoin.rf_paramids` is a list.** One entry today; the cross-segment
  join would carry the Params of the filters it feeds, local and remote, and
  the publish step (`ExecHashJoinPublishRuntimeFilter()`) is the one place
  that would also serialize and send.
- **Sizing is reproducible.** Every builder sizes its filter from the plan's
  estimate and `whpg_runtime_filter_max_size`, which is synchronized to the
  segments, so filters built for the same join on different segments have
  the same block count and merge; the header's `flags` word (0 today, equal
  on both sides of a merge) is where a partitioned or otherwise special
  filter will say so.
- **The scan side needs no new contract to wait.** A filter that arrives
  after the scan has started applies from the next row; a scan that should
  rather wait for it -- the design's `wait_timeout` -- would do so where the
  scan is opened (`SeqNext()`), before the first row, with the Param still
  NULL meaning "go ahead without it".

What the cross-segment work adds is therefore the transport (a plan rewrite or
an interconnect channel), the aggregation of per-segment filters, the wait /
timeout policy at the receiving scan, and the planner's decision of when the
network saving pays for the transport; the filter, the clause, the scan side
and the EXPLAIN stay.

## Limitations and follow-ups

- The survivor estimate, the join's output rows over the probe's rows, is an
  upper bound: a one-to-many join (duplicate keys on the hash side) can put
  it over the selectivity gate although few probe rows have a match, and
  then no filter is planned. A distinct-key estimate for the inner side
  would tighten it.
- A join on several keys gets a filter over one of them. A filter over the
  combined hash value (what the table uses) would be tighter where no key
  alone is selective -- the pairs of two unselective keys can be rare -- once
  the clause takes a key list; it would drop probe rows a single key's filter
  passes, at the price of hashing every key of every probe row.
- The per-key estimate behind the choice assumes the inner side's keys are a
  subset of the probe's (a foreign key's), spread evenly, and takes the inner
  side's distinct values as its rows when the key is not a column with
  statistics; a selective predicate on the probe scan's other columns leaves
  it optimistic (the probe's distinct values are those of the whole column).
  The run-time switch-off covers what the estimate misses.
- Under the Postgres planner a partitioned fact is an Append of scans; the
  key would need translating per partition through the AppendRelInfos. GPORCA's
  DynamicSeqScan is covered.
- The raw-datum fast path is Phase A's. Once the density check hands a
  segment file back to the ordinary scan, and on heap and AO row tables, the
  clause runs through `ExecQual()` and a function call per row; a batch probe
  for the ordinary scan's qual is what the dense case above still needs.
- Only the key column's own blocks take the fast path; a key of a type outside
  the integer and datetime families (text, numeric, uuid) goes row by row.
- The filter's keys and size are not yet reported by EXPLAIN ANALYZE.
