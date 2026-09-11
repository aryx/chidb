# Plan: possible next extensions beyond the four course assignments

**Status:** not started. Written 2026-09-11, as a gap analysis after finishing
the range-seek extension (see plan_chidb_implementation.md and changes.txt's
2.0 entry) -- a place to record candidate next steps before picking one,
rather than re-deriving this list from scratch next session.

All four course assignments (B-Tree, DBM, code generator, query optimizer)
are implemented and `make check` is green. Everything below is optional
follow-on work, roughly ordered by effort (smallest first). See
plan_chidb_implementation.md's own "Not implemented" section for the fuller
technical detail behind each of these; this file is the shortlist plus
effort/value notes for deciding what to do next, not a duplicate writeup.

## 1. Fix the CHIDB_EDUPLICATE / CHIDB_EMISUSE error-code collision -- DONE (2026-09-11)

`CHIDB_EDUPLICATE` (chidbInt.h, private) and `CHIDB_EMISUSE` (chidb.h,
public) were both numerically `8` in the pre-existing constant tables, so a
genuine duplicate-key error (e.g. `CREATE INDEX` over a column that isn't
actually unique) got reported by the shell as "API used incorrectly"
instead. Fixed two ways: `chidb_dbm_op_Insert`/`chidb_dbm_op_IdxInsert`
(dbm-ops.c) now translate `CHIDB_EDUPLICATE` to the public, documented
`CHIDB_ECONSTRAINT` right at the DBM/API boundary (matching
architecture.html's own public return-code table, which already says
`CHIDB_ECONSTRAINT` means "SQL statement failed because of a constraint
violation" -- exactly this case), and `CHIDB_EDUPLICATE` itself was
renumbered to `11` in chidbInt.h so it no longer aliases a public code at
all, independent of whether some future caller also forgets to translate
it. Verified against the shell directly (`CREATE INDEX` + a duplicate
`INSERT` now prints the constraint-violation message) and `make check`
(124/124, unchanged).

## 2. Two-sided range seeks -- DONE (2026-09-11)

`WHERE indexedcol > 10 AND indexedcol < 20` -- a bounded range on the same
indexed column -- previously fell back to a full scan with both conjuncts
checked as ordinary filters, because index-seek planning required the
seekable side's condition to be exactly one comparison (`ncmp == 1`,
checked in codegen.c before even looking at what the comparison is).

Implemented as designed: a new `detect_range_pair()` (codegen.c) recognizes
exactly "two conjuncts, same indexed column, one a lower bound (`>`/`>=`)
and the other an upper bound (`<`/`<=`)" and normalizes them so the caller
always seeks forward from the lower bound (`SeekGt`/`SeekGe`) and stops the
walk once the upper bound fails on a visited row -- a direct per-row check
(there's no opcode to read an index cursor's own key column, only its
PKey, so the check reads the already-derived table row instead), routed to
the same "stop, don't retry" target a natural `Next`/`Prev` exhaustion
would use (`addr_tail` for `codegen_select_indexed` and the single-table
path; `patch_die` for a join's outer/left side, since it's the outermost
loop; a new `patch_stop_inner`/`addr_done_inner` pairing for a join's
inner/right side, since failing there must only end *that* outer row's
inner walk, not the whole query -- unlike the outer side, there may be
more outer rows left to try). `SideAccess` gained a `cmp2` field (NULL
unless this side is a two-sided range) and two new literal registers
(`r_lit1b`/`r_lit2b`) to carry both sides' index code.

Verified against the shell directly (`EXPLAIN` confirms a single
`SeekGt`/`SeekGe` plus a bounded check, no scan) for: multiple matches,
a range collapsing to one row, a logically-empty range (`>30 AND <10`),
a range with no data in it despite the seek succeeding, and -- the
trickiest case -- a two-sided range on a join's *inner* side, confirmed to
correctly reset and re-bound per outer row rather than only working for
the first one, including the case where one outer row's inner walk is
correctly abandoned (bound failure) while later outer rows still produce
matches. Also verified both sides two-sided at once, which compiles to
zero scan/Rewind instructions at all -- just two independent SeekGt+bound
pairs plus the join-pair check.

7 new DBMF fixtures: `sql-select-016.dbmf` through `018.dbmf` (single-table:
multi-match, single-row via inclusive both-bounds, and a seek-succeeds/
zero-rows miss) and `join-range-010.dbmf` through `013.dbmf` (join: outer
two-sided, inner two-sided demonstrating the per-outer-row reset, both
sides two-sided together, and a seek-succeeds/immediate-upper-bound-fail
miss). `make check`: 131/131 (up from 124), all green.

## 3. DELETE statement support

The parser already accepts `DELETE FROM table WHERE ...` (`sql.y`'s
`delete_from`, `STMT_DELETE`), but codegen rejects it outright
(`chidb_stmt_codegen`'s `default:` case, `codegen.c`: "DELETE, and anything
else the parser accepts: out of scope") -- and there is no B-Tree
cell-removal function anywhere in `btree.c`/`btree.h` at all, nor a DBM
delete opcode. This wasn't part of the four assignments' scope
(`assignment_codegen.html` only asks for schema loading, SELECT, INSERT,
CREATE TABLE, and NATURAL JOIN), so it's a genuinely new feature, not a
gap in what was assigned.

Effort: large. Needs: a B-Tree delete algorithm (cell removal, and internal
node underflow handling -- borrowing from or merging with a sibling, the
usual B-Tree deletion complexity that insertion's split logic doesn't
mirror for free), at least one new DBM opcode wired through dbm-ops.c and
dbm.c, and DELETE codegen. Worth scoping carefully before starting --
this is the single biggest item on this list.

## 4. 3-way / general joins

Only two-way `NATURAL JOIN` of two bare tables is supported --
`codegen_select_join` rejects anything where either side of the
`SRA_NATURAL_JOIN` isn't a bare `SRA_TABLE` optionally wrapped in one
`SRA_SELECT`. No `JOIN ... ON`/`USING`, outer joins, 3-way joins, or
`UNION`/`INTERSECT`/`EXCEPT`. Sigma-pushing itself is equally narrow: it
only recognizes one `Select` directly over one `NaturalJoin` of two bare
tables, so extending joins without extending the optimizer alongside them
would just mean the new join shapes never get their WHERE conditions
pushed down.

Effort: large. The two pieces (codegen and optimizer) need to grow
together, and a 3-way join in particular raises real design questions
(join order/associativity) that two-way NATURAL JOIN never had to answer.

Update (2026-09-11): turns out smaller than this made it sound --
sql.y's `table` grammar rule is already left-recursive and already
parses a chain of any number of joins, of any mix of join types
(`SRA_JOIN`/`SRA_NATURAL_JOIN`/outer-join nodes), so an N-way *inner*
join isn't blocked on the parser at all, only on `codegen_select_join`'s
own narrow two-table check. See
[plan_sqlite_extensions.md](plan_sqlite_extensions.md)'s #7 for the
fuller breakdown (N-way inner joins first, then `ON`/`USING`, then
`OUTER JOIN`, each a smaller independent step) and its "key insight"
section for what else the front end already parses but codegen ignores.

## 5. O(1)-amortized cursors (explicit bonus-credit item)

`dbm-cursor.c`'s cursor implementation materializes a whole B-Tree's
in-order entry sequence into a sorted array per cursor on open, rather
than an incremental, stack-based O(log n)-space traversal.
`assignment_dbm.html` step 3 explicitly sanctions the materialized
approach as a valid first approximation, but also names the amortized-O(1)
`Next` version as the bonus-credit shape.

Effort: large, and fairly self-contained (mostly confined to
dbm-cursor.[ch] -- the opcode handlers in dbm-ops.c that call into cursor
functions shouldn't need to change, only the cursor's own internals and
the `chidb_dbm_cursor_t` struct). The most "pure" of the remaining items in
that it doesn't touch codegen or the optimizer at all, so lowest risk of
destabilizing anything already working -- but real B-Tree stack-based
traversal (tracking a path of (node, cell index) pairs, handling descent
into children and backtracking on exhaustion) is nontrivial to get right,
especially around split-during-modification invalidating an open cursor's
path.

## Recommendation if resumed cold

Start with #1 (trivial, isolated) to warm up, then #2 (moderate,
continues the just-finished range-seek work while the DBM seek-opcode
mental model is still fresh) before either of the two large items (#3
DELETE, #4 general joins) or the self-contained-but-substantial #5.
