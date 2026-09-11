# Plan: porting more real-SQLite features into chidb (didactic, incremental)

**Status:** not started. Written 2026-09-11, as a companion to
[plan_extensions.md](plan_extensions.md) (which covers finishing the four
course assignments' own loose ends) -- this file is specifically about
features real SQLite has that chidb doesn't, picked and scoped the way the
original four assignments were: each step small enough to build and test on
its own, building on the previous step, reusing chidb's existing
architecture rather than reinventing it. See
[notes_sqlite.txt](notes_sqlite.txt) for the fuller architectural
comparison this plan assumes as background.

Not a "make chidb into SQLite" plan -- most of these are chosen because
they teach a specific real database concept cheaply within chidb's existing
DBM/B-Tree/SRA architecture. A few real SQLite features (Tier 4 below) are
included anyway, with a note on why they're probably *not* worth doing in
this codebase, so the decision not to attempt them is recorded rather than
silently forgotten.

## Key insight: the SQL front end already outpaces codegen

Before ranking anything by size, it's worth being explicit about something
that changes the cost of a lot of this list: `src/libchisql`'s grammar
(`sql.y`) and the `SRA_t`/`Expression_t`/`Condition_t` trees it builds
already model far more SQL than `codegen.c` currently compiles. This
repo's own four-assignments implementation (see plan_chidb_implementation.md)
only ever taught `codegen_select`/`codegen_select_join` to look at a
`Project(Select?(Table | NaturalJoin(Table,Table)))` shape with an AND-chain
`WHERE`. Concretely, all of the following are *already parsed and sitting in
the tree*, unused by codegen:
- `SRA_Project_t` (sra.h) has `order_by`, `group_by`, `distinct`, and
  `asc_desc` fields, populated by `SRA_applyOption`/`SRA_makeDistinct`
  (sql.y's `opt_options`/`opt_distinct` rules) -- codegen never reads them.
- `SRA_JOIN`/`SRA_LEFT_OUTER_JOIN`/`SRA_RIGHT_OUTER_JOIN`/
  `SRA_FULL_OUTER_JOIN` nodes, and `SRA_UNION`/`SRA_INTERSECT`/`SRA_EXCEPT`,
  all exist and are built by the parser (sql.y's `table`/`join`/`select`
  rules) -- codegen only ever recognizes `SRA_NATURAL_JOIN`.
- sql.y's `table` rule is already left-recursive (`table default_join
  table_ref ...` / `table join table_ref ...`), so a chain of *any* number
  of joins, of any mix of join types, already parses into a tree of
  `SRA_JOIN`/`SRA_NATURAL_JOIN`/outer-join nodes -- two-way NATURAL JOIN
  isn't a parser limitation, it's purely how far `codegen_select_join`
  currently reaches into that tree.
- `Expression_t` (expression.h) already has arithmetic (`+ - * /`, unary
  negation, string `CONCAT`) and the five aggregate functions (`COUNT SUM
  AVG MIN MAX`, as `TERM_FUNC`/`Func`) as real tree nodes -- codegen's
  `ColResolver`/`ResolvedCmp` machinery only ever unwraps a bare column
  reference or literal, nothing else.
- `Condition_t` (condition.h) already has `RA_COND_OR`, `RA_COND_NOT`, and
  `RA_COND_IN` (a literal list, via sql.y's `in_statement`) alongside
  `RA_COND_AND` -- codegen's `flatten_conjuncts`/`resolve_conjuncts` only
  ever walk an AND-chain of comparisons.
- `Constraint_t` (column.h's `enum constraint_type`; sql.y's `constraint`
  rule) already parses `CONS_NOT_NULL`, `CONS_UNIQUE`, `CONS_CHECK`,
  `CONS_DEFAULT`, and `CONS_FOREIGN_KEY` -- `codegen_create_table` only
  ever reads `CONS_PRIMARY_KEY`, to find which column is the B-Tree key;
  everything else is parsed, stored in the schema, and never looked at
  again.

So a large chunk of Tier 1 below is genuinely just "teach codegen to read a
field that's already there" -- not new parser or B-Tree work. That's the
main reason this plan orders things the way it does.

Three things real SQLite has that are *not* in the grammar at all yet, and
so need lexer/parser work too, not just codegen: `LIMIT`/`OFFSET`, `UPDATE`,
`CREATE VIEW`, and `BEGIN`/`COMMIT`/`ROLLBACK` (`IN (SELECT ...)` is parsed
but the parser itself already prints "WARNING: IN SELECT statement not yet
supported" -- the original authors flagged this one themselves). Called out
individually below.

## Tier 1 -- codegen-only or near enough, do these first

### 1. A general expression evaluator

Foundational for almost everything else in this tier. Real SQLite compiles
an arbitrary expression tree to a sequence of DBM/VDBE instructions leaving
its value in a register (`sqlite3ExprCode`); chidb's codegen currently has
no equivalent at all -- `emit_rcol` only ever loads a bare column's current
value, and every comparison is hand-resolved by `resolve_comparison`
against a literal. dbm-types.h's `FOREACH_OP` list (the DBM's full 37
opcodes) has none of `Add`/`Subtract`/`Multiply`/`Divide`/`Concat` --
chidb's DBM was never asked to do arithmetic, only comparisons -- so this
genuinely needs a handful of new opcodes, not just new codegen. Didactic
version: a few small new opcodes mirroring the shape of `dbm-ops.c`'s
existing register opcodes (each one reads one or two register operands
and writes a result register), plus a recursive `codegen_expr(stmt, expr,
out_reg)` that walks `Expression_t`, emitting those new opcodes for
`EXPR_PLUS`/`MINUS`/`MULTIPLY`/`DIVIDE`/`CONCAT`, and falling through to
the existing column/literal loading for `EXPR_TERM`. Once this exists,
SELECT's projection list stops being limited to bare columns (`SELECT
price * qty FROM orders` becomes possible), and it's the shared building
block Tier 1's other items reuse.

Effort: small-medium. A handful of new DBM opcodes plus the codegen.c
walker. Depends on nothing else in this list.

### 2. Generalize WHERE to OR / NOT / IN

`resolve_conjuncts` currently rejects anything that isn't a flat AND-chain
of `column OP literal`. `RA_COND_OR`/`RA_COND_NOT`/`RA_COND_IN` are already
parsed (condition.h) but never reach codegen. Once #1 exists, a boolean
expression compiler is a natural sibling: emit each side into a register,
combine with real `Not`/short-circuit jump logic (SQLite's VDBE, and
chidb's own DBM, both already have the primitive `Eq`/`Ne`/... opcodes with
jump targets -- this is about *composing* them for `OR`/`NOT`, not new
opcodes). `IN (v1, v2, ...)` desugars trivially to `col = v1 OR col = v2
OR ...` for a first pass, matching real SQLite's own fallback for a short,
non-indexable `IN` list.

Effort: medium. The interesting design question is whether this *replaces*
`ResolvedCmp`/index-seek planning (which needs to keep recognizing the
narrow "one indexed comparison" shape to still plant a seek) or sits
alongside it as a fallback for whatever seek-planning doesn't handle --
almost certainly the latter, so this only kicks in once seek-planning has
already tried and failed to find a single indexable top-level comparison.

### 3. ORDER BY, then LIMIT/OFFSET

`SRA_Project_t.order_by`/`asc_desc` are already populated, unread. chidb's
own cursor implementation (dbm-cursor.c) already materializes a whole
table's rows into an array before iterating -- reusing exactly that idea
for ORDER BY (materialize the *result set*, sort the array by the ORDER BY
expression(s) using #1's evaluator, then emit rows from the sorted array)
is a very small step conceptually, and worth explicitly teaching as "this
is what real SQLite's ephemeral-sorter does for a query with no matching
index, just without spilling to disk for large results."

`LIMIT n [OFFSET m]` isn't in the grammar at all yet -- needs a lexer
token, a small grammar addition attaching it to `select_statement`
alongside `opt_options`, and in codegen is just a register counting emitted
rows with a conditional jump to the halt/close tail once the count is
reached (or, with OFFSET, once far enough past it). Natural pairing with
ORDER BY since "top N" queries are the most common reason to combine them,
but independently useful (and independently small) even without it.

Effort: small (LIMIT/OFFSET) to medium (ORDER BY, mainly for choosing a
reasonable in-memory sort and getting multi-key comparison right). Depends
on #1 for sorting by a computed expression, not needed for sorting by a
bare column.

### 4. DISTINCT

`SRA_Project_t.distinct` is already populated, unread. Cheapest possible
follow-on to #3: sort the materialized result set (reusing #3's
machinery), then skip a row if it's identical to the previous one --
exactly how a sort-based DISTINCT works in a real database without a
hash-based alternative.

Effort: small. Depends on #3.

### 5. Aggregate functions and GROUP BY

`COUNT`/`SUM`/`AVG`/`MIN`/`MAX` are already parsed as `TERM_FUNC` nodes,
and `SRA_Project_t.group_by` is already populated -- both unread by
codegen. Without an index on the GROUP BY column(s), real SQLite's
simplest strategy is also sort-then-group (there's a hash-based
alternative too, but the sort-based one is the didactic fit here): sort
the materialized rows by the GROUP BY expression (reusing #3 again), then
walk the sorted array accumulating one running aggregate per group,
emitting a result row each time the group key changes. `SELECT COUNT(*)
FROM t` with no GROUP BY at all is the degenerate single-group case and a
good first milestone before tackling real grouping.

Effort: medium. Depends on #1 (evaluating the aggregated expression per
row) and #3 (the sort machinery). The accumulator state itself (running
sum/count/min/max per group) is a small, self-contained new piece --
either a few dedicated registers reset at each group boundary, or (closer
to how real SQLite's `AggStep`/`AggFinal` opcodes work) two new DBM
opcodes if it's worth teaching that mechanism explicitly rather than
special-casing it in codegen.

### 6. UNION / INTERSECT / EXCEPT

`SRA_UNION`/`SRA_INTERSECT`/`SRA_EXCEPT` nodes already exist and are built
by sql.y's `select_combo` rule -- entirely unread by codegen. Once #3/#4's
"materialize the whole result set, then sort it" machinery exists, all
three set operations are the same sort-merge idea taught in an
undergrad algorithms class: run both sides, sort+dedup each (all three
operators imply DISTINCT semantics in standard SQL), then merge-walk the
two sorted arrays together -- keep-if-in-either for UNION, keep-if-in-both
for INTERSECT, keep-if-only-in-left for EXCEPT.

Effort: medium. Depends on #3/#4.

### 7. JOIN ... ON/USING, multi-way joins, and OUTER JOIN

As the "key insight" section above lays out, the grammar already builds
an arbitrary-length chain of `SRA_JOIN`/`SRA_NATURAL_JOIN`/outer-join
nodes -- `codegen_select_join` is what's narrow, not the parser. Natural
progression: first generalize the existing two-way-NATURAL-JOIN-only
nested-loop codegen to walk a *chain* of joins left-to-right (still
inner/NATURAL only, but N-way instead of fixed at 2 -- this alone
addresses plan_extensions.md's "3-way / general joins" item, and turns
out to be smaller than that file estimates once the grammar support is
accounted for); then add `JOIN ... ON <condition>` (`SRA_JOIN`'s
`opt_cond`, `JOIN_COND_ON`) as a nested loop with the ON condition
checked as a filter, same shape as a pushed-down WHERE conjunct; then
`USING (col, ...)` as sugar for an ON-equality per named column; then
outer joins, which need one new idea: track whether the current outer-loop
row found *any* match on the inner side, and if the whole inner loop
finishes having found none, emit the outer row once anyway with the inner
side's columns forced to NULL -- a good, concrete lesson in how NULL
generation actually works in a real join executor.
Sigma-pushing (optimizer.c) needs extending alongside this: pushing a
WHERE conjunct below an outer join changes the query's meaning (a
condition on the *inner* side of a LEFT JOIN can't be pushed the same way
a NATURAL JOIN's can, since it would incorrectly filter out the
NULL-padded non-matching rows) -- a genuinely interesting, subtle
correctness point worth its own writeup when this is tackled.

Effort: large overall, but cleanly separable into the sub-steps above
(N-way inner NATURAL/JOIN first, ON/USING second, OUTER third), each
independently small and testable, in the same spirit the four original
assignments and this fork's own NATURAL-JOIN/sigma-pushing/index-seek
extensions were built.

## Tier 2 -- medium, need some parser and/or storage work

### 8. Constraint enforcement at INSERT time (NOT NULL, UNIQUE, PRIMARY KEY)

`Constraint_t` already parses `CONS_UNIQUE`, `CONS_NOT_NULL`, `CONS_CHECK`,
`CONS_DEFAULT` (column.h) -- `codegen_create_table` reads only
`CONS_PRIMARY_KEY` (to pick the B-Tree key column) and discards the rest.
`codegen_insert` could check the constraint list already sitting in
`chidb_schema_item_t` (or walk the table's own `Create_t` if the schema
item needs a field added) before emitting the `Insert`/`IdxInsert`
opcodes: NOT NULL as a register NULL-check, UNIQUE and PRIMARY KEY as a
`Seek` against the relevant index/table first, refusing the insert on a
hit (`CHIDB_EDUPLICATE` already exists as a constant -- see
plan_extensions.md's #1 for its numbering collision with
`CHIDB_EMISUSE`, worth fixing before leaning on it here). None of that
matters, though, without a way to actually *refuse* the insert and
report why: `Op_Halt`'s handler (dbm-ops.c) is deliberately a two-line
`stmt->pc = stmt->endOp` with no error-code/message plumbing at all --
its own comment cites assignment_dbm.html explicitly not requiring one.
A first real prerequisite for this whole item, then, is giving `Halt` (or
a new dedicated opcode) a way to carry an error code out to `chidb_step`,
which currently has no path for "the DBM program itself decided to fail
partway through" at all. No CHECK expression evaluation without #1 (Tier
1) first, since a CHECK clause is an arbitrary boolean expression over
the row being inserted.

Effort: small-medium per constraint kind; genuinely useful before
DELETE/UPDATE (below) exist, since insert-time correctness is worth
having even in a database that otherwise can't modify existing rows.

### 9. UPDATE statement

Not in the grammar at all -- needs an `UPDATE` token, a grammar rule
(`UPDATE table_name SET col = expr, ... [WHERE cond]`, `Update_t` and
`STMT_UPDATE` mirroring `Delete_t`/`STMT_DELETE`'s existing shape), and
new codegen. Interesting design question specific to chidb's simplified,
fixed-shape records (no overflow pages, so a record's *encoded size* can
still change if a variable-length TEXT column's new value is longer or
shorter than its old one): updating a non-key column whose new encoded
size matches the old cell size can overwrite the cell in place; anything
else (a size change, or updating the primary-key column itself) has to
fall back to delete-then-reinsert, which means UPDATE's own codegen wants
DELETE's B-Tree removal primitive (#10) to exist first, even for pure
non-key updates, unless an in-place-only "fast path" is deliberately
scoped as a first milestone before the general case.

Effort: medium, but genuinely blocked on #10 for the general (variable
record size, or key column) case; an in-place same-size fast path could
land independently first as a smaller milestone.

### 10. DELETE statement and B-Tree cell removal

Already covered in depth in plan_extensions.md's #3 -- included here only
for the dependency arrows to UPDATE (#9) above and cost-aware planning
(#12) below, both of which want *some* form of "the B-Tree can shrink."
No change to that file's effort estimate (large): a real B-Tree delete
needs underflow handling (borrowing from / merging with a sibling node),
which insertion's split logic doesn't hand you symmetrically for free.

### 11. Views

`CREATE VIEW name [(cols)] AS <select>` isn't in the grammar yet, but is
one of the more self-contained additions on this whole list: a view is
purely a SQL-layer concept, no B-Tree or DBM changes at all. Store the
view's name and its `SELECT` text in the schema table (reusing the
existing `sql` column `chidb_schema_item_t` already carries for CREATE
TABLE/INDEX statements), and at `chidb_prepare` time, when a `FROM`
clause names something that resolves to a view rather than a table,
re-parse its stored SELECT and substitute the resulting `SRA_t` subtree
in place of what would otherwise have been an `SRA_TABLE` node -- the
same "macro expansion before codegen ever runs" trick real SQLite uses
for views internally. Column-reference resolution needs the substituted
subtree's own column names threaded through correctly (the view's
projected column list, possibly aliased via `CREATE VIEW v(a,b) AS ...`),
which is the fiddly part; codegen itself needs no awareness that a view
was ever involved, since by the time it runs the tree looks like an
ordinary nested SELECT.

Effort: medium. Needs FROM's `table_ref` production, or the schema
lookup that follows it, to be able to say "this name is a view, not a
table" and branch accordingly -- currently `table_name` always resolves
straight to `chidb_schema_find_table`.

### 12. Cost-aware index/join-order choice

Only makes sense once #7 lands N-way joins -- with only two-way joins,
"pick a join order" is a non-question, which is exactly why this is
ranked after it. Real SQLite's actual query planner (`whereScanner`/
`sqlite3WhereBegin` machinery, plus `ANALYZE`-gathered `sqlite_stat1`
row-count/selectivity statistics) is the single most complex subsystem
in the whole codebase -- not something to reproduce. A didactic-scale
version: (a) a cheap row-count estimate per table, computed by walking
the B-Tree's leaf level once (or cached on the schema item and refreshed
lazily) rather than a real statistics table; (b) for an N-way join, a
simple greedy join-order heuristic -- e.g. "always join in the order that
keeps the smallest number of rows live at each step," estimating a join's
output size as (say) the smaller side's row count when the join column is
indexed on the other side, or the product of both sides' row counts
otherwise; (c) extending the existing hard-coded "always seek if an index
matches" rule (optimizer.c) to actually compare estimated seek cost
against estimated scan cost, so a range condition matching most of a
table correctly prefers a scan. This is the smallest *reasonable*
approximation of "cost-based query planning" that's still teaching the
real idea (cardinality estimation driving both access-method and
join-order choice) rather than either skipping it or trying to build
SQLite's actual planner.

Effort: large, and the one item on this whole list with the most open
design questions rather than a mostly-mechanical port -- worth a proper
design pass of its own before starting, likely its own
plan_query_planner.md if/when it's picked up.

## Tier 3 -- big, capstone-style, multi-step

### 13. Transactions (BEGIN / COMMIT / ROLLBACK) via a simple rollback journal

The single biggest documented gap called out by chidb's own authors
(SIGCSE paper section 6: "the biggest gap in chidb ... no support for
concurrency or ACID transactions whatsoever"; fileformat.html: "a user
is assumed to have exclusive access"). Real SQLite historically shipped
exactly the mechanism worth teaching here before WAL existed: a rollback
journal. Didactic version, as its own small sequence of milestones (this
item is really "assignment 5" in scope, not a single step):
1. `BEGIN`/`COMMIT`/`ROLLBACK` grammar + a `chidb_stmt`-level or
   `chidb`-level transaction-state flag (none of these three tokens
   exist in sql.y yet).
2. On the *first* write inside a transaction to a given page, `pager.c`
   copies that page's pre-modification bytes to a journal file before
   the write proceeds (SQLite's own `sqlite3PagerWrite` does exactly
   this) -- chidb's Pager already mediates every page read/write, which
   is what makes this interceptable in one place rather than scattered
   across every btree.c call site that mutates a page.
2. `ROLLBACK` replays the journal backward, restoring every journaled
   page's pre-image, then truncates/deletes the journal file; `COMMIT`
   just deletes the journal file, keeping every write already made.
3. Crash recovery: on `chidb_open`, check for a leftover ("hot") journal
   file next to the database file -- its mere presence, from a process
   that died mid-transaction, means the same replay `ROLLBACK` does
   needs to run automatically before the database is usable again. This
   is the single most illustrative part of the whole feature: it's the
   concrete mechanism behind "a database survives a crash," not just an
   abstract guarantee.

Explicitly *not* in scope for a didactic version: real concurrency
(multiple connections, file locking states) -- single-writer,
single-reader-at-a-time is a reasonable simplification to keep, matching
chidb's existing "a user is assumed to have exclusive access" stance;
just add the crash-durability half without the concurrency half.

Effort: large, multi-milestone; mostly self-contained to pager.c plus a
thin grammar/codegen layer for the three new statements, which is a nice
property (it doesn't destabilize btree.c/dbm-ops.c/codegen.c's existing,
already-tested logic).

### 14. Overflow pages

fileformat.html lists this among its own explicit simplifying
assumptions ("no overflow pages ... a record can't exceed a page's
usable size"). Real SQLite spills a large record's tail across a chain
of overflow pages when it doesn't fit in a B-Tree leaf cell. Didactic
version: extend the leaf-cell format with an optional "next overflow
page" pointer (mirroring fileformat.html's own table-leaf cell layout,
just with a length threshold past which the record's tail moves off-page
instead of failing), and thread overflow-awareness through
`getCell`/`insertCell` (`btree.c`) and wherever a record's bytes are
decoded (`record.c`). Good, self-contained capstone if the interest is
specifically "go deeper on storage" rather than SQL surface area, and
unlike Tier 1/2 doesn't depend on anything else in this plan.

Effort: large. Confined to btree.c/record.c (and the corresponding
check_btree tests), no codegen/optimizer changes needed at all, which
makes it a good candidate to hand to a different contributor/session
than whoever's working through Tier 1/2, if this ever gets picked up in
parallel.

## Tier 4 -- real SQLite features, probably not worth it here

Recorded so the decision not to attempt these is explicit, not an
oversight.

- **Multi-connection concurrency / file locking** (SQLite's five-state
  locking protocol, shared/reserved/pending/exclusive). Real SQLite's
  second-most-complex subsystem after the query planner; chidb's spec
  explicitly assumes exclusive single-user access throughout, and Tier
  3's transaction plan deliberately keeps that assumption. Only worth
  reconsidering if this project's actual goal ever shifts toward
  concurrent access rather than durability.
- **WAL mode**. A real alternative/upgrade to Tier 3's rollback journal,
  but a second whole mechanism (a write-ahead log file plus a shared
  in-memory index into it) for the same durability goal Tier 3 already
  covers didactically with the simpler, older mechanism. Not worth
  teaching both.
- **Triggers and foreign-key referential integrity enforcement**. Real
  SQL features, but a large SQL-surface and codegen addition (trigger
  bodies are themselves arbitrary SQL statements needing their own
  codegen path; FK enforcement needs cross-table checks at INSERT/UPDATE/
  DELETE time) for a course-scale codebase whose SQL subset (a single
  4-byte-int primary key per table, no schema migrations) doesn't have
  a lot of the referential-integrity complexity real FK enforcement
  exists to handle anyway.
- **PRAGMA statements / VACUUM / auto-vacuum / a real free-list**.
  Real SQLite features, but low teaching value relative to effort --
  mostly bookkeeping (reclaiming pages freed by DELETE/UPDATE once #10
  exists) rather than a new concept. Worth a `chidb_schema_item_t`-style
  free-list *if and only if* #10/#14 land and page churn actually starts
  mattering in practice; not worth building ahead of that need.

## Suggested order if resumed cold

Tier 1, roughly in the numbered order above (each item's own effort note
says what it depends on within the tier) -- it's the cheapest, most
self-contained, and most directly reuses chidb's existing architecture
(the materialize-and-sort cursor idea in particular pays for itself three
times over, in #3/#4/#5/#6). Tier 2's constraint enforcement (#8) is a
reasonable next step after Tier 1 even before DELETE/UPDATE exist, since
insert-time correctness stands alone. Save Tier 3 for whenever there's
room for a multi-session capstone-sized effort, matching how this repo's
own four original assignments were each themselves multi-day efforts, not
single-sitting ones.
