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

## 1. Fix the CHIDB_EDUPLICATE / CHIDB_EMISUSE error-code collision

`CHIDB_EDUPLICATE` (chidbInt.h, private) and `CHIDB_EMISUSE` (chidb.h,
public) are both numerically `8` in the pre-existing constant tables, so a
genuine duplicate-key error (e.g. `CREATE INDEX` over a column that isn't
actually unique) gets reported by the shell as "API used incorrectly"
instead. Cosmetic, but a real (if minor) bug.

Effort: small. Risk: need to check nothing else relies on the exact existing
numbering of either constant table before renumbering one of them.

## 2. Two-sided range seeks

`WHERE indexedcol > 10 AND indexedcol < 20` -- a bounded range on the same
indexed column -- currently falls back to a full scan with both conjuncts
checked as ordinary filters, because index-seek planning requires the
seekable side's condition to be exactly one comparison (`ncmp == 1`,
checked in codegen.c before even looking at what the comparison is). This
is a natural continuation of the range-seek work in the 2.0 entry: reusing
`IndexSeekKind`/`SideAccess`/`ACC_RANGESEEK` but starting the walk from a
lower-bound seek and stopping it once the upper bound is exceeded (or vice
versa for a descending walk), instead of relying on the cursor's own
exhaustion.

Effort: moderate. Touches `codegen_select_indexed` and
`codegen_select_join` (`resolve_conjuncts`-adjacent code needs to recognize
the two-sided-same-column shape and pass both bounds through), plus new
tests mirroring the existing `sql-select-01[2-5].dbmf` /
`join-range-*.dbmf` pattern.

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
