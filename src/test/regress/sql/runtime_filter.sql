-- Runtime filters for hash joins.
--
-- Once a hash join has built its hash table, it hands the probe side's scan a
-- Bloom filter over the join keys (an exec Param read by a probe clause the
-- planner put in the scan's qual), and the scan drops the rows that
-- cannot have a match before it assembles them. On an AOCO table the clause
-- leads a Staged Scan, so those rows' other columns are never decompressed.
-- Only joins whose probe side is a scan in the same slice get one: no Motion
-- in between, and only join types that drop unmatched probe rows.
--
-- Part 1 is the Postgres planner, Part 2 GPORCA. Two helpers keep the matrix
-- legible: rf_planned(q) says whether EXPLAIN shows a Runtime Filter line,
-- rf_same(q) whether q (yielding one text value) answers the same with the
-- feature on and off.

CREATE SCHEMA runtime_filter;
SET search_path = runtime_filter;

CREATE FUNCTION rf_planned(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
	l text;
	found bool := false;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Runtime Filter:%' THEN
			found := true;
		END IF;
	END LOOP;
	RETURN found;
END $$;

CREATE FUNCTION rf_same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
	a text;
	b text;
BEGIN
	SET LOCAL whpg_enable_runtime_filter = on;
	EXECUTE q INTO a;
	SET LOCAL whpg_enable_runtime_filter = off;
	EXECUTE q INTO b;
	RETURN a IS NOT NULL AND a = b;
END $$;

-- The dimension: 5000 keys, a flag that keeps 2% of them, a bigint copy of
-- the key and a text one for cross-type and text joins.
CREATE TABLE rf_dim(k int, flag int, kb bigint, name text) DISTRIBUTED BY (k);
INSERT INTO rf_dim SELECT i, i % 50, i, 'name_' || i FROM generate_series(1, 5000) i;

-- The fact tables: 200000 rows, keys over 1..6000 so that some never match,
-- one key in a thousand NULL; one table per access method.
CREATE TABLE rf_fact_heap(id int, k int, amt int, tag text, pad text) DISTRIBUTED BY (id);
INSERT INTO rf_fact_heap
	SELECT i,
		   CASE WHEN i % 997 = 0 THEN NULL ELSE (i * 7919) % 6000 + 1 END,
		   i % 1000,
		   'name_' || ((i * 7919) % 6000 + 1),
		   repeat('x', 20)
	  FROM generate_series(1, 200000) i;
CREATE TABLE rf_fact_ao(LIKE rf_fact_heap) USING ao_row DISTRIBUTED BY (id);
CREATE TABLE rf_fact_aoco(LIKE rf_fact_heap)
	USING ao_column WITH (compresstype = zstd, compresslevel = 1) DISTRIBUTED BY (id);
INSERT INTO rf_fact_ao SELECT * FROM rf_fact_heap;
INSERT INTO rf_fact_aoco SELECT * FROM rf_fact_heap;
ANALYZE rf_dim;
ANALYZE rf_fact_heap;
ANALYZE rf_fact_ao;
ANALYZE rf_fact_aoco;

-- the tables are small; let a probe of a few thousand rows qualify
SET whpg_runtime_filter_min_probe_rows = 1000;

----------------------------------------------------------------------
-- Part 1: the Postgres planner
----------------------------------------------------------------------
SET optimizer = off;

-- An AOCO fact: the filter clause leads a Staged Scan on the join key.
EXPLAIN (COSTS OFF)
SELECT count(*), sum(f.amt) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.k WHERE d.flag = 3;
SELECT count(*), sum(f.amt) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.k WHERE d.flag = 3;
SELECT rf_same($$SELECT md5(string_agg(f.id::text, ',' ORDER BY f.id)) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.k WHERE d.flag = 3$$);

-- Heap and AO row: the clause runs as an ordinary qual.
EXPLAIN (COSTS OFF)
SELECT count(*) FROM rf_fact_heap f JOIN rf_dim d ON f.k = d.k WHERE d.flag = 3;
SELECT rf_same($$SELECT md5(string_agg(f.id::text, ',' ORDER BY f.id)) FROM rf_fact_heap f JOIN rf_dim d ON f.k = d.k WHERE d.flag = 3$$);
EXPLAIN (COSTS OFF)
SELECT count(*) FROM rf_fact_ao f JOIN rf_dim d ON f.k = d.k WHERE d.flag = 3;
SELECT rf_same($$SELECT md5(string_agg(f.id::text, ',' ORDER BY f.id)) FROM rf_fact_ao f JOIN rf_dim d ON f.k = d.k WHERE d.flag = 3$$);

-- A predicate of the fact's own as well. The scan keeps its own staged read
-- of amt and the filter is left out: the Staged Scan model takes a
-- two-column predicate at its pessimistic estimate (see the star join
-- below). amt < 50 keeps 5% of the rows; 10% would sit on the model's
-- threshold and flip with the sample ANALYZE takes.
EXPLAIN (COSTS OFF)
SELECT count(*) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.k WHERE d.flag = 3 AND f.amt < 50;
SELECT rf_same($$SELECT md5(string_agg(f.id::text, ',' ORDER BY f.id)) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.k WHERE d.flag = 3 AND f.amt < 50$$);

-- The fact's columns in the output: the filter column is not among them.
SELECT rf_same($$SELECT md5(string_agg(f.id::text || f.amt::text || f.pad, ',' ORDER BY f.id)) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.k WHERE d.flag = 3$$);

-- Join types: only those that drop unmatched probe rows.
SELECT rf_planned($$SELECT count(*) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.k WHERE d.flag = 3$$) AS inner_join;
SELECT rf_planned($$SELECT count(*) FROM rf_fact_aoco f LEFT JOIN rf_dim d ON f.k = d.k AND d.flag = 3$$) AS left_join;
SELECT rf_planned($$SELECT count(*) FROM rf_fact_aoco f WHERE EXISTS (SELECT 1 FROM rf_dim d WHERE d.k = f.k AND d.flag = 3)$$) AS semi_join;
SELECT rf_planned($$SELECT count(*) FROM rf_fact_aoco f WHERE NOT EXISTS (SELECT 1 FROM rf_dim d WHERE d.k = f.k AND d.flag = 3)$$) AS anti_join;
SELECT rf_planned($$SELECT count(*) FROM rf_fact_aoco f WHERE f.k NOT IN (SELECT d.k FROM rf_dim d WHERE d.flag = 3)$$) AS not_in;
SELECT rf_same($$SELECT count(*)::text FROM rf_fact_aoco f LEFT JOIN rf_dim d ON f.k = d.k AND d.flag = 3$$) AS left_join;
SELECT rf_same($$SELECT count(*)::text FROM rf_fact_aoco f WHERE EXISTS (SELECT 1 FROM rf_dim d WHERE d.k = f.k AND d.flag = 3)$$) AS semi_join;
SELECT rf_same($$SELECT count(*)::text FROM rf_fact_aoco f WHERE NOT EXISTS (SELECT 1 FROM rf_dim d WHERE d.k = f.k AND d.flag = 3)$$) AS anti_join;
SELECT rf_same($$SELECT count(*)::text FROM rf_fact_aoco f WHERE f.k NOT IN (SELECT d.k FROM rf_dim d WHERE d.flag = 3)$$) AS not_in;

-- Cross-type (int4 = int8) and text keys: the probe hashes with its own
-- type's function, which the hash opfamily keeps consistent with the inner's.
EXPLAIN (COSTS OFF)
SELECT count(*) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.kb WHERE d.flag = 3;
SELECT rf_same($$SELECT md5(string_agg(f.id::text, ',' ORDER BY f.id)) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.kb WHERE d.flag = 3$$);
EXPLAIN (COSTS OFF)
SELECT count(*) FROM rf_fact_aoco f JOIN rf_dim d ON f.tag = d.name WHERE d.flag = 3;
SELECT rf_same($$SELECT md5(string_agg(f.id::text, ',' ORDER BY f.id)) FROM rf_fact_aoco f JOIN rf_dim d ON f.tag = d.name WHERE d.flag = 3$$);

-- An array key. The key is hashed by the column's own type (its elements'
-- hash function), so the clause carries that type, not the polymorphic
-- anyarray GPORCA relabels the key to; the planner tests the key's type as
-- the probe function will, so the two cannot disagree.
CREATE TABLE rf_dim_arr(k int, karr int[], flag int) DISTRIBUTED BY (k);
INSERT INTO rf_dim_arr SELECT i, ARRAY[i, i % 7], i % 50 FROM generate_series(1, 5000) i;
CREATE TABLE rf_fact_arr(id int, karr int[]) DISTRIBUTED BY (id);
INSERT INTO rf_fact_arr
	SELECT i, ARRAY[(i * 7919) % 6000 + 1, ((i * 7919) % 6000 + 1) % 7]
	  FROM generate_series(1, 20000) i;
ANALYZE rf_dim_arr;
ANALYZE rf_fact_arr;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM rf_fact_arr f JOIN rf_dim_arr d ON f.karr = d.karr WHERE d.flag = 3;
SELECT rf_same($$SELECT md5(string_agg(f.id::text, ',' ORDER BY f.id)) FROM rf_fact_arr f JOIN rf_dim_arr d ON f.karr = d.karr WHERE d.flag = 3$$) AS array_key;

-- A probe under a Motion gets no filter: both sides large, the fact is
-- redistributed to the other table's key.
SELECT rf_planned($$SELECT count(*) FROM rf_fact_aoco f JOIN rf_fact_heap g ON f.k = g.id$$) AS probe_under_motion;
SELECT rf_same($$SELECT count(*)::text FROM rf_fact_aoco f JOIN rf_fact_heap g ON f.k = g.id$$) AS probe_under_motion;

-- Too many probe rows would match: no filter.
SELECT rf_planned($$SELECT count(*) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.k$$) AS unselective;

-- A filter that turns out useless is switched off as the scan runs: the
-- selectivity gate is lifted so that one is planned although five probe rows
-- in six have a match, and the switch-off ratio lowered so that the scan
-- gives the filter up after its first 65536 rows. The answer is the same.
SET whpg_runtime_filter_max_selectivity = 1.0;
SET whpg_runtime_filter_disable_ratio = 0.5;
SELECT rf_planned($$SELECT count(*) FROM rf_fact_heap f JOIN rf_dim d ON f.k = d.k$$) AS forced;
SELECT rf_same($$SELECT md5(string_agg(f.id::text, ',' ORDER BY f.id)) FROM rf_fact_heap f JOIN rf_dim d ON f.k = d.k$$) AS useless_filter_heap;
SELECT rf_same($$SELECT md5(string_agg(f.id::text, ',' ORDER BY f.id)) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.k$$) AS useless_filter_aoco;
RESET whpg_runtime_filter_disable_ratio;
RESET whpg_runtime_filter_max_selectivity;

-- The switch.
SET whpg_enable_runtime_filter = off;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.k WHERE d.flag = 3;
RESET whpg_enable_runtime_filter;

-- rf_fault_hits(fault, q): how many times the segments hit a fault point of
-- the executor's while q ran with the filter on.
CREATE FUNCTION rf_fault_hits(fault text, q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
	hits bigint;
BEGIN
	SET LOCAL whpg_enable_runtime_filter = on;
	PERFORM public.gp_inject_fault_infinite(fault, 'skip', dbid)
	   FROM gp_segment_configuration WHERE content > -1 AND role = 'p';
	EXECUTE q;
	SELECT sum(substring(public.gp_inject_fault(fault, 'status', dbid)
						 from 'num times hit:''(\d+)''')::bigint)
	  INTO hits
	  FROM gp_segment_configuration WHERE content > -1 AND role = 'p';
	PERFORM public.gp_inject_fault(fault, 'reset', dbid)
	   FROM gp_segment_configuration WHERE content > -1 AND role = 'p';
	RETURN hits;
END $$;

-- Statistics far off on the build side: planned on 10 rows, the filter gets
-- 200000 keys at run time, far too many for the 64 kB allowed, and is
-- dropped; the join still answers right.
CREATE TABLE rf_stale(k int) DISTRIBUTED BY (k);
INSERT INTO rf_stale SELECT i FROM generate_series(1, 10) i;
ANALYZE rf_stale;
INSERT INTO rf_stale SELECT i FROM generate_series(11, 200000) i;
SET whpg_runtime_filter_max_size = 64;
SELECT rf_planned($$SELECT count(*) FROM rf_fact_aoco f JOIN rf_stale s ON f.k = s.k$$) AS planned_on_stale_stats;
SELECT rf_fault_hits('runtime_filter_published', $$SELECT count(*) FROM rf_fact_aoco f JOIN rf_stale s ON f.k = s.k$$) AS dropped_none_published;
SELECT count(*) FROM rf_fact_aoco f JOIN rf_stale s ON f.k = s.k;
SELECT rf_same($$SELECT count(*)::text FROM rf_fact_aoco f JOIN rf_stale s ON f.k = s.k$$);
RESET whpg_runtime_filter_max_size;

-- Statistics far off, within the limit: planned on 10 rows, the filter gets
-- 20010 keys, too many for the 8 kB it was sized at, and is built again
-- from the hash table at the 64 kB they call for, once per segment; the
-- probe's 66000 rows a segment are far more than those keys, so it is
-- published.
CREATE TABLE rf_stale_small(k int) DISTRIBUTED BY (k);
INSERT INTO rf_stale_small SELECT i FROM generate_series(1, 10) i;
ANALYZE rf_stale_small;
INSERT INTO rf_stale_small SELECT i FROM generate_series(11, 20010) i;
SELECT rf_fault_hits('runtime_filter_rebuilt', $$SELECT count(*) FROM rf_fact_aoco f JOIN rf_stale_small s ON f.k = s.k$$) AS rebuilt_one_per_segment;
SELECT rf_fault_hits('runtime_filter_published', $$SELECT count(*) FROM rf_fact_aoco f JOIN rf_stale_small s ON f.k = s.k$$) AS published_one_per_segment;
SELECT rf_same($$SELECT count(*)::text FROM rf_fact_aoco f JOIN rf_stale_small s ON f.k = s.k$$) AS rebuilt;

-- The same build against a small probe: 20010 keys against the dimension's
-- 1700 rows a segment would let every probe row through, so the filter,
-- rebuilt or not, is not published.
SELECT rf_fault_hits('runtime_filter_withheld', $$SELECT count(*) FROM rf_dim d JOIN rf_stale_small s ON d.k = s.k$$) AS withheld_one_per_segment;
SELECT rf_fault_hits('runtime_filter_published', $$SELECT count(*) FROM rf_dim d JOIN rf_stale_small s ON d.k = s.k$$) AS withheld_none_published;
SELECT rf_same($$SELECT count(*)::text FROM rf_dim d JOIN rf_stale_small s ON d.k = s.k$$) AS withheld;

-- Statistics far above the rows: analyzed at 200000 keys, a hundred are
-- left, and the 128 kB filter sized for the 33000 a segment the planner
-- expects is folded down to the 8 kB the twenty keys a segment need, once
-- per segment. (The predicate is on another column, so it does not carry
-- over to the fact through the join; both sides are distributed on the
-- key, so neither moves.)
CREATE TABLE rf_shrunk(k int, grp int) DISTRIBUTED BY (k);
INSERT INTO rf_shrunk SELECT i, i % 2 FROM generate_series(1, 200000) i;
ANALYZE rf_shrunk;
DELETE FROM rf_shrunk WHERE k > 100;
SET whpg_runtime_filter_max_selectivity = 1.0;
SELECT rf_fault_hits('runtime_filter_folded', $$SELECT count(*) FROM rf_fact_aoco f JOIN rf_shrunk s ON f.id = s.k WHERE s.grp = 0$$) AS folded_one_per_segment;
SELECT rf_same($$SELECT count(*)::text FROM rf_fact_aoco f JOIN rf_shrunk s ON f.id = s.k WHERE s.grp = 0$$) AS folded;
RESET whpg_runtime_filter_max_selectivity;

-- Rescans. A hash join on the inner side of a nested loop runs again for
-- every outer row; it keeps its hash table across those runs when its own
-- inner side takes nothing from the outer row (ExecReScanHashJoin()), and
-- the filter with it, so the probe scan is read again through the same
-- filter. The dimension is replicated so that the join runs on the
-- segments with no Motion inside it (a Motion under a rescanned node is
-- materialized, and the join would run once); Material is off and the join
-- above is an inequality, so the three-row side stays outer. The fault
-- point runtime_filter_published fires once per filter built, and
-- rf_filters_built(q) sums it over the segments: one per segment here,
-- not one per outer row.
CREATE TABLE rf_dim_rep (LIKE rf_dim) DISTRIBUTED REPLICATED;
INSERT INTO rf_dim_rep SELECT * FROM rf_dim;
ANALYZE rf_dim_rep;
CREATE FUNCTION rf_filters_built(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
	hits bigint;
BEGIN
	PERFORM public.gp_inject_fault_infinite('runtime_filter_published', 'skip', dbid)
	   FROM gp_segment_configuration WHERE content > -1 AND role = 'p';
	EXECUTE q;
	SELECT sum(substring(public.gp_inject_fault('runtime_filter_published', 'status', dbid)
						 from 'num times hit:''(\d+)''')::bigint)
	  INTO hits
	  FROM gp_segment_configuration WHERE content > -1 AND role = 'p';
	PERFORM public.gp_inject_fault('runtime_filter_published', 'reset', dbid)
	   FROM gp_segment_configuration WHERE content > -1 AND role = 'p';
	RETURN hits;
END $$;
SET enable_material = off;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM (SELECT DISTINCT flag FROM rf_dim_rep WHERE flag < 3) v
	JOIN (SELECT f.id FROM rf_fact_aoco f JOIN rf_dim_rep d ON f.k = d.k WHERE d.flag = 3) q ON q.id % 100 >= v.flag * 30;
SELECT rf_filters_built($$SELECT count(*) FROM (SELECT DISTINCT flag FROM rf_dim_rep WHERE flag < 3) v JOIN (SELECT f.id FROM rf_fact_aoco f JOIN rf_dim_rep d ON f.k = d.k WHERE d.flag = 3) q ON q.id % 100 >= v.flag * 30$$) AS filters_built_one_per_segment;
SELECT rf_same($$SELECT count(*)::text FROM (SELECT DISTINCT flag FROM rf_dim_rep WHERE flag < 3) v JOIN (SELECT f.id FROM rf_fact_aoco f JOIN rf_dim_rep d ON f.k = d.k WHERE d.flag = 3) q ON q.id % 100 >= v.flag * 30$$) AS rescan;
RESET enable_material;

-- A cursor fetched in pieces: the filter is in place before the first row.
BEGIN;
DECLARE rf_cur CURSOR FOR SELECT f.id FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.k WHERE d.flag = 3 ORDER BY f.id;
FETCH 3 FROM rf_cur;
FETCH 3 FROM rf_cur;
CLOSE rf_cur;
COMMIT;

-- A hash join over a non-strict hash operator matches NULL keys (it hashes
-- them as 0, ExecHashGetHashValue()), which the filter would reject: such a
-- join gets no filter. The operator is IS NOT DISTINCT FROM in disguise, in
-- plpgsql so that the planner cannot inline it back into one; nested loops
-- are off so that the hash join is chosen. 200 fact rows have a NULL key.
CREATE FUNCTION rf_nseq(int, int) RETURNS bool LANGUAGE plpgsql IMMUTABLE CALLED ON NULL INPUT AS
$$ BEGIN RETURN $1 IS NOT DISTINCT FROM $2; END $$;
CREATE OPERATOR === (LEFTARG = int, RIGHTARG = int, FUNCTION = rf_nseq, COMMUTATOR = ===, HASHES, RESTRICT = eqsel, JOIN = eqjoinsel);
CREATE OPERATOR FAMILY rf_nsfam USING hash;
CREATE OPERATOR CLASS rf_nsop FOR TYPE int USING hash FAMILY rf_nsfam AS OPERATOR 1 ===, FUNCTION 1 hashint4(int);
CREATE TABLE rf_dim_null(k int) DISTRIBUTED RANDOMLY;
INSERT INTO rf_dim_null VALUES (NULL), (17);
ANALYZE rf_dim_null;
SET enable_nestloop = off;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM rf_fact_heap f JOIN rf_dim_null d ON f.k === d.k;
SELECT rf_planned($$SELECT count(*) FROM rf_fact_heap f JOIN rf_dim_null d ON f.k === d.k$$) AS non_strict_operator;
SELECT count(*) FILTER (WHERE f.k IS NULL) AS null_matches, count(*) FILTER (WHERE f.k = 17) AS key_matches
  FROM rf_fact_heap f JOIN rf_dim_null d ON f.k === d.k;
SELECT rf_planned($$SELECT count(*) FROM rf_fact_heap f JOIN rf_dim_null d ON f.k = d.k$$) AS strict_operator;
RESET enable_nestloop;

-- The Staged Scan's fast path. A NULL-free block of the join key column in
-- the original datum layout is tested on its raw datum array: each value is
-- hashed and looked up in the filter directly, no tuple formed, no
-- ExecQual(); the fault point StagedScan_PhaseA_FastBlock fires once per
-- block so tested. The keys the dimension holds are clustered so that Phase
-- A finds its survivors in a few blocks and runs to the end of the file. The
-- fault functions live outside this schema.
SET search_path = runtime_filter, public;
CREATE TABLE rf_fast(k int, k8 bigint, dt date, pay text)
	USING ao_column WITH (compresstype = zstd, compresslevel = 1) DISTRIBUTED BY (k);
INSERT INTO rf_fast
	SELECT i, i, date '2020-01-01' + i, 'p' || i FROM generate_series(1, 400000) i;
CREATE TABLE rf_fast_dim(k int, k8 bigint, dt date) DISTRIBUTED BY (k);
INSERT INTO rf_fast_dim SELECT i, i, date '2020-01-01' + i FROM generate_series(1, 4000) i;
ANALYZE rf_fast;
ANALYZE rf_fast_dim;
CREATE FUNCTION rf_fast_blocks(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
	hits bigint;
BEGIN
	PERFORM gp_inject_fault('StagedScan_PhaseA_FastBlock', 'skip', '', '', 'rf_fast', 1, -1, 0, dbid)
	   FROM gp_segment_configuration WHERE content > -1 AND role = 'p';
	EXECUTE q;
	SELECT sum(substring(gp_inject_fault('StagedScan_PhaseA_FastBlock', 'status', dbid)
						 from 'num times hit:''(\d+)''')::bigint)
	  INTO hits
	  FROM gp_segment_configuration WHERE content > -1 AND role = 'p';
	PERFORM gp_inject_fault('StagedScan_PhaseA_FastBlock', 'reset', dbid)
	   FROM gp_segment_configuration WHERE content > -1 AND role = 'p';
	RETURN hits;
END $$;
EXPLAIN (COSTS OFF)
SELECT count(f.pay) FROM rf_fast f JOIN rf_fast_dim d ON f.k = d.k;
-- an int4 key, hashed inline; the same join without a filter stages nothing
SELECT rf_fast_blocks($$SELECT count(f.pay) FROM rf_fast f JOIN rf_fast_dim d ON f.k = d.k$$) > 0 AS fast_int4;
SELECT rf_same($$SELECT md5(string_agg(f.k::text, ',' ORDER BY f.k)) FROM rf_fast f JOIN rf_fast_dim d ON f.k = d.k$$) AS fast_int4;
SET whpg_enable_runtime_filter = off;
SELECT rf_fast_blocks($$SELECT count(f.pay) FROM rf_fast f JOIN rf_fast_dim d ON f.k = d.k$$) AS no_filter_no_fast_blocks;
RESET whpg_enable_runtime_filter;
-- an int8 key, hashed inline
SELECT rf_fast_blocks($$SELECT count(f.pay) FROM rf_fast f JOIN rf_fast_dim d ON f.k8 = d.k8$$) > 0 AS fast_int8;
SELECT rf_same($$SELECT md5(string_agg(f.k::text, ',' ORDER BY f.k)) FROM rf_fast f JOIN rf_fast_dim d ON f.k8 = d.k8$$) AS fast_int8;
-- a date key, hashed through the operator's hash function
SELECT rf_fast_blocks($$SELECT count(f.pay) FROM rf_fast f JOIN rf_fast_dim d ON f.dt = d.dt$$) > 0 AS fast_date;
SELECT rf_same($$SELECT md5(string_agg(f.k::text, ',' ORDER BY f.k)) FROM rf_fast f JOIN rf_fast_dim d ON f.dt = d.dt$$) AS fast_date;
-- a clause of the scan's own on the key column beside the filter: both on
-- the fast path
SELECT rf_fast_blocks($$SELECT count(f.pay) FROM rf_fast f JOIN rf_fast_dim d ON f.k = d.k WHERE f.k < 3000$$) > 0 AS fast_with_range;
SELECT rf_same($$SELECT md5(string_agg(f.k::text, ',' ORDER BY f.k)) FROM rf_fast f JOIN rf_fast_dim d ON f.k = d.k WHERE f.k < 3000$$) AS fast_with_range;
SET search_path = runtime_filter;

-- A star join: the fact joined to two dimensions, each selective, is a
-- chain of hash joins over one scan. The upper join's key is a column of the
-- fact too, so its filter is tested at the scan as well: two Runtime Filter
-- lines on the heap fact, the more selective one first. On the AOCO fact the
-- first filter leads a Staged Scan, and a second filter column would cost it
-- that (the Staged Scan model takes a multi-column predicate at its
-- pessimistic estimate), so the second filter is left out and the scan
-- keeps its staged read of the first key. Through a LEFT join (fact outer)
-- the upper filter still reaches the scan, while the LEFT join itself gets
-- none; a key taken from a lower join's inner side gets none.
CREATE FUNCTION rf_filters(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE
	l text;
	n int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Runtime Filter:%' THEN
			n := n + 1;
		END IF;
	END LOOP;
	RETURN n;
END $$;
CREATE TABLE rf_dim_amt(amt int, flag int) DISTRIBUTED REPLICATED;
INSERT INTO rf_dim_amt SELECT i, i % 20 FROM generate_series(0, 999) i;
ANALYZE rf_dim_amt;
-- (k and amt are both derived from the row number, so only amt values 8 and
-- 18 modulo 20 occur among the rows whose k has flag 3: flag 8 keeps half
-- of them, flags 0-7 none)
EXPLAIN (COSTS OFF)
SELECT count(*) FROM rf_fact_heap f JOIN rf_dim d ON f.k = d.k JOIN rf_dim_amt a ON f.amt = a.amt WHERE d.flag = 3 AND a.flag = 8;
SELECT count(*) FROM rf_fact_heap f JOIN rf_dim d ON f.k = d.k JOIN rf_dim_amt a ON f.amt = a.amt WHERE d.flag = 3 AND a.flag = 8;
SELECT rf_filters($$SELECT count(*) FROM rf_fact_heap f JOIN rf_dim d ON f.k = d.k JOIN rf_dim_amt a ON f.amt = a.amt WHERE d.flag = 3 AND a.flag = 8$$) AS star_two_filters_heap;
SELECT rf_same($$SELECT md5(string_agg(f.id::text, ',' ORDER BY f.id)) FROM rf_fact_heap f JOIN rf_dim d ON f.k = d.k JOIN rf_dim_amt a ON f.amt = a.amt WHERE d.flag = 3 AND a.flag = 8$$) AS star_two_filters_heap;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.k JOIN rf_dim_amt a ON f.amt = a.amt WHERE d.flag = 3 AND a.flag = 8;
SELECT rf_filters($$SELECT count(*) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.k JOIN rf_dim_amt a ON f.amt = a.amt WHERE d.flag = 3 AND a.flag = 8$$) AS star_keeps_staged_scan;
SELECT rf_same($$SELECT md5(string_agg(f.id::text, ',' ORDER BY f.id)) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.k JOIN rf_dim_amt a ON f.amt = a.amt WHERE d.flag = 3 AND a.flag = 8$$) AS star_keeps_staged_scan;
SELECT rf_filters($$SELECT count(*) FROM rf_fact_aoco f LEFT JOIN rf_dim d ON f.k = d.k AND d.flag = 3 JOIN rf_dim_amt a ON f.amt = a.amt WHERE a.flag = 1$$) AS through_left_join;
SELECT rf_same($$SELECT md5(string_agg(f.id::text || coalesce(d.k::text, 'x'), ',' ORDER BY f.id)) FROM rf_fact_aoco f LEFT JOIN rf_dim d ON f.k = d.k AND d.flag = 3 JOIN rf_dim_amt a ON f.amt = a.amt WHERE a.flag = 1$$) AS through_left_join;
SELECT rf_filters($$SELECT count(*) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.k JOIN rf_dim_amt a ON d.flag = a.amt WHERE d.flag < 3 AND a.flag = 1$$) AS key_from_inner_side;
SELECT rf_same($$SELECT md5(string_agg(f.id::text, ',' ORDER BY f.id)) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.k JOIN rf_dim_amt a ON d.flag = a.amt WHERE d.flag < 3 AND a.flag = 1$$) AS key_from_inner_side;

-- A join on several keys takes a filter over one of them: the key whose
-- values the inner side covers the smallest share of, by the statistics (the
-- inner side's distinct values of the key over the probe column's). rf_dim_mk
-- has every k of the fact, so a filter over k would pass every row, and 20 of
-- the fact's 1000 amt values, so the filter is over amt -- on whichever side
-- of the AND it stands. rf_dim_mk_k is the other way round: 60 k values
-- (every hundredth) with 100 amt values each, so its filter is over k, a
-- column with NULLs on both sides, which the filter rejects as the join does.
CREATE TABLE rf_dim_mk(k int, amt int) DISTRIBUTED REPLICATED;
INSERT INTO rf_dim_mk
	SELECT i, CASE WHEN i % 300 = 0 THEN NULL ELSE (i % 20) * 50 END
	  FROM generate_series(1, 6000) i;
CREATE TABLE rf_dim_mk_k(k int, amt int) DISTRIBUTED REPLICATED;
INSERT INTO rf_dim_mk_k
	SELECT j * 100 + 1, a FROM generate_series(0, 59) j CROSS JOIN generate_series(0, 99) a
	UNION ALL SELECT NULL, i FROM generate_series(0, 9) i;
ANALYZE rf_dim_mk;
ANALYZE rf_dim_mk_k;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM rf_fact_heap f JOIN rf_dim_mk d ON f.k = d.k AND f.amt = d.amt;
SELECT count(*) FROM rf_fact_heap f JOIN rf_dim_mk d ON f.k = d.k AND f.amt = d.amt;
SELECT rf_same($$SELECT md5(string_agg(f.id::text, ',' ORDER BY f.id)) FROM rf_fact_heap f JOIN rf_dim_mk d ON f.k = d.k AND f.amt = d.amt$$) AS two_keys_second_chosen;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM rf_fact_aoco f JOIN rf_dim_mk d ON f.amt = d.amt AND f.k = d.k;
SELECT rf_same($$SELECT md5(string_agg(f.id::text, ',' ORDER BY f.id)) FROM rf_fact_aoco f JOIN rf_dim_mk d ON f.amt = d.amt AND f.k = d.k$$) AS two_keys_first_chosen;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM rf_fact_aoco f JOIN rf_dim_mk_k d ON f.amt = d.amt AND f.k = d.k;
SELECT count(*) FROM rf_fact_aoco f JOIN rf_dim_mk_k d ON f.amt = d.amt AND f.k = d.k;
SELECT rf_same($$SELECT md5(string_agg(f.id::text, ',' ORDER BY f.id)) FROM rf_fact_aoco f JOIN rf_dim_mk_k d ON f.amt = d.amt AND f.k = d.k$$) AS two_keys_nulls;

-- The filter's allocation failing, as an out-of-memory error would: the
-- error is the statement's, the session goes on, the next run is right.
SELECT public.gp_inject_fault('runtime_filter_create', 'error', dbid)
FROM gp_segment_configuration WHERE role = 'p' AND content >= 0;
SELECT count(*) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.k WHERE d.flag = 3;
SELECT public.gp_inject_fault('runtime_filter_create', 'reset', dbid)
FROM gp_segment_configuration WHERE role = 'p' AND content >= 0;
SELECT count(*) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.k WHERE d.flag = 3;

-- Why a join got no filter: EXPLAIN says, when asked. The last two are joins
-- on two keys: one where neither key alone would drop enough rows (rf_dim's
-- k and name cover 5000 of the fact's 6000), one whose probe has no
-- statistics to choose a key by.
SET gp_autostats_mode = none;
CREATE TABLE rf_fact_nostats(LIKE rf_fact_heap) DISTRIBUTED BY (id);
INSERT INTO rf_fact_nostats SELECT * FROM rf_fact_heap;
RESET gp_autostats_mode;
SET whpg_runtime_filter_explain = on;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM rf_fact_aoco f LEFT JOIN rf_dim d ON f.k = d.k AND d.flag = 3;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.k;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM rf_fact_aoco f JOIN rf_fact_heap g ON f.k = g.id;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.k AND f.tag = d.name;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM rf_fact_nostats f JOIN rf_dim_mk d ON f.k = d.k AND f.amt = d.amt;
RESET whpg_runtime_filter_explain;

----------------------------------------------------------------------
-- Part 2: GPORCA
----------------------------------------------------------------------
SET optimizer = on;

EXPLAIN (COSTS OFF)
SELECT count(*), sum(f.amt) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.k WHERE d.flag = 3;
SELECT count(*), sum(f.amt) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.k WHERE d.flag = 3;
SELECT rf_same($$SELECT md5(string_agg(f.id::text, ',' ORDER BY f.id)) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.k WHERE d.flag = 3$$);
EXPLAIN (COSTS OFF)
SELECT count(*) FROM rf_fact_heap f JOIN rf_dim d ON f.k = d.k WHERE d.flag = 3;
SELECT rf_same($$SELECT md5(string_agg(f.id::text, ',' ORDER BY f.id)) FROM rf_fact_heap f JOIN rf_dim d ON f.k = d.k WHERE d.flag = 3$$);
SELECT rf_same($$SELECT md5(string_agg(f.id::text, ',' ORDER BY f.id)) FROM rf_fact_ao f JOIN rf_dim d ON f.k = d.k WHERE d.flag = 3$$);
SELECT rf_same($$SELECT md5(string_agg(f.id::text, ',' ORDER BY f.id)) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.k WHERE d.flag = 3 AND f.amt < 50$$);

SELECT rf_planned($$SELECT count(*) FROM rf_fact_aoco f LEFT JOIN rf_dim d ON f.k = d.k AND d.flag = 3$$) AS left_join;
SELECT rf_planned($$SELECT count(*) FROM rf_fact_aoco f WHERE EXISTS (SELECT 1 FROM rf_dim d WHERE d.k = f.k AND d.flag = 3)$$) AS semi_join;
SELECT rf_planned($$SELECT count(*) FROM rf_fact_aoco f WHERE NOT EXISTS (SELECT 1 FROM rf_dim d WHERE d.k = f.k AND d.flag = 3)$$) AS anti_join;
SELECT rf_same($$SELECT count(*)::text FROM rf_fact_aoco f LEFT JOIN rf_dim d ON f.k = d.k AND d.flag = 3$$) AS left_join;
SELECT rf_same($$SELECT count(*)::text FROM rf_fact_aoco f WHERE EXISTS (SELECT 1 FROM rf_dim d WHERE d.k = f.k AND d.flag = 3)$$) AS semi_join;
SELECT rf_same($$SELECT count(*)::text FROM rf_fact_aoco f WHERE NOT EXISTS (SELECT 1 FROM rf_dim d WHERE d.k = f.k AND d.flag = 3)$$) AS anti_join;

SELECT rf_same($$SELECT md5(string_agg(f.id::text, ',' ORDER BY f.id)) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.kb WHERE d.flag = 3$$) AS cross_type;
SELECT rf_same($$SELECT md5(string_agg(f.id::text, ',' ORDER BY f.id)) FROM rf_fact_aoco f JOIN rf_dim d ON f.tag = d.name WHERE d.flag = 3$$) AS text_key;
-- The array key GPORCA relabels to anyarray: the filter is on the column,
-- hashed by its own type. GPORCA expects every probe row to match (it has
-- no statistics on an array column), so the selectivity gate is lifted.
SET whpg_runtime_filter_max_selectivity = 1.0;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM rf_fact_arr f JOIN rf_dim_arr d ON f.karr = d.karr WHERE d.flag = 3;
SELECT rf_same($$SELECT md5(string_agg(f.id::text, ',' ORDER BY f.id)) FROM rf_fact_arr f JOIN rf_dim_arr d ON f.karr = d.karr WHERE d.flag = 3$$) AS array_key;
RESET whpg_runtime_filter_max_selectivity;
SELECT rf_filters($$SELECT count(*) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.k JOIN rf_dim_amt a ON f.amt = a.amt WHERE d.flag = 3 AND a.flag = 8$$) AS star_keeps_staged_scan;
SELECT rf_same($$SELECT md5(string_agg(f.id::text, ',' ORDER BY f.id)) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.k JOIN rf_dim_amt a ON f.amt = a.amt WHERE d.flag = 3 AND a.flag = 8$$) AS star_keeps_staged_scan;
SELECT rf_planned($$SELECT count(*) FROM rf_fact_aoco f JOIN rf_fact_heap g ON f.k = g.id$$) AS probe_under_motion;
SELECT rf_planned($$SELECT count(*) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.k$$) AS unselective;
-- Rescans: a correlated subquery runs again for every outer row. GPORCA
-- keeps the outer reference above the join, so the hash join inside keeps
-- its table and filter across the runs, and the probe scan is read again
-- through the filter. The fact is replicated so that the subquery runs on
-- one segment with no Motion. (The Postgres planner materializes every scan
-- of a correlated subplan and plans no filter there; its rescans are those
-- of a nested loop, Part 1.)
CREATE TABLE rf_fact_rep (LIKE rf_fact_heap)
	USING ao_column WITH (compresstype = zstd, compresslevel = 1) DISTRIBUTED REPLICATED;
INSERT INTO rf_fact_rep SELECT * FROM rf_fact_heap;
ANALYZE rf_fact_rep;
EXPLAIN (COSTS OFF)
SELECT v.flag, (SELECT count(*) FROM rf_fact_rep f JOIN rf_dim_rep d ON f.k = d.k WHERE d.flag = 3 AND f.amt > v.flag * 10)
FROM (SELECT DISTINCT flag FROM rf_dim_rep WHERE flag < 3) v ORDER BY 1;
SELECT rf_filters_built($$SELECT v.flag, (SELECT count(*) FROM rf_fact_rep f JOIN rf_dim_rep d ON f.k = d.k WHERE d.flag = 3 AND f.amt > v.flag * 10) FROM (SELECT DISTINCT flag FROM rf_dim_rep WHERE flag < 3) v ORDER BY 1$$) AS filters_built_once;
SELECT rf_same($$SELECT md5(string_agg(flag::text || ':' || c::text, ',' ORDER BY flag)) FROM (SELECT v.flag, (SELECT count(*) FROM rf_fact_rep f JOIN rf_dim_rep d ON f.k = d.k WHERE d.flag = 3 AND f.amt > v.flag * 10) c FROM (SELECT DISTINCT flag FROM rf_dim_rep WHERE flag < 3) v) s$$) AS rescan;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM rf_fact_aoco f JOIN rf_dim_mk d ON f.k = d.k AND f.amt = d.amt;
SELECT rf_same($$SELECT md5(string_agg(f.id::text, ',' ORDER BY f.id)) FROM rf_fact_aoco f JOIN rf_dim_mk d ON f.k = d.k AND f.amt = d.amt$$) AS two_keys_second_chosen;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM rf_fact_heap f JOIN rf_dim_mk_k d ON f.amt = d.amt AND f.k = d.k;
SELECT rf_same($$SELECT md5(string_agg(f.id::text, ',' ORDER BY f.id)) FROM rf_fact_heap f JOIN rf_dim_mk_k d ON f.amt = d.amt AND f.k = d.k$$) AS two_keys_nulls;
SELECT rf_planned($$SELECT count(*) FROM rf_fact_aoco f JOIN rf_dim d ON f.k = d.k AND f.tag = d.name$$) AS two_keys_neither_selective;

-- Two replicated tables: GPORCA joins them on one segment, and in that
-- slice its row estimates count a replicated table once per segment of the
-- cluster. The gates read the rows the one segment sees: a build side of
-- 50000 ids fits a 64 kB filter, which three times as many would not.
SET whpg_runtime_filter_max_size = '64kB';
SET whpg_runtime_filter_explain = on;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM rf_fact_rep f JOIN rf_fact_rep g ON f.id = g.id WHERE g.amt < 250;
RESET whpg_runtime_filter_explain;
SELECT rf_same($$SELECT md5(string_agg(f.id::text, ',' ORDER BY f.id)) FROM rf_fact_rep f JOIN rf_fact_rep g ON f.id = g.id WHERE g.amt < 250$$) AS replicated_build;
RESET whpg_runtime_filter_max_size;

-- A partitioned AOCO fact: the filter leads the dynamic scan's Staged Scan
-- in every partition.
CREATE TABLE rf_fact_part(id int, k int, amt int, tag text)
	USING ao_column WITH (compresstype = zstd, compresslevel = 1)
	DISTRIBUTED BY (id)
	PARTITION BY RANGE (id) (START (1) END (200001) EVERY (50000));
INSERT INTO rf_fact_part SELECT id, k, amt, tag FROM rf_fact_heap;
ANALYZE rf_fact_part;
EXPLAIN (COSTS OFF)
SELECT count(*), sum(f.amt) FROM rf_fact_part f JOIN rf_dim d ON f.k = d.k WHERE d.flag = 3;
SELECT count(*), sum(f.amt) FROM rf_fact_part f JOIN rf_dim d ON f.k = d.k WHERE d.flag = 3;
SELECT rf_same($$SELECT md5(string_agg(f.id::text, ',' ORDER BY f.id)) FROM rf_fact_part f JOIN rf_dim d ON f.k = d.k WHERE d.flag = 3$$) AS partitioned;

-- A useless filter on the partitioned fact is switched off once, for every
-- partition's scan: the tally is the filter's own, not each scan's, so the
-- partitions (50000 rows each, fewer than the 65536 the switch waits for)
-- do not each start it over. Forty of the fifty flags keep four fifths of
-- the dimension, which GPORCA still broadcasts (the whole dimension it
-- would redistribute the fact to), and two probe rows in three match.
SET whpg_runtime_filter_max_selectivity = 1.0;
SET whpg_runtime_filter_disable_ratio = 0.5;
SELECT rf_planned($$SELECT count(*) FROM rf_fact_part f JOIN rf_dim d ON f.k = d.k WHERE d.flag < 40$$) AS forced_partitioned;
SELECT rf_same($$SELECT md5(string_agg(f.id::text, ',' ORDER BY f.id)) FROM rf_fact_part f JOIN rf_dim d ON f.k = d.k WHERE d.flag < 40$$) AS useless_filter_partitioned;
RESET whpg_runtime_filter_disable_ratio;
RESET whpg_runtime_filter_max_selectivity;

RESET optimizer;
RESET whpg_runtime_filter_min_probe_rows;
RESET search_path;
DROP SCHEMA runtime_filter CASCADE;
