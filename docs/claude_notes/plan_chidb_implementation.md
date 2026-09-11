# Plan: implement the chidb assignment (B-Tree + DBM + code generator)

**Status:** in progress. Started 2026-09-10.

**Correction (2026-09-10, same day):** the initial CLAUDE.md I wrote for this
repo claimed `btree.c`/`pager.c` were a finished reference implementation and
only `dbm-ops.c`/`optimizer.c`/`codegen.c` were stubs. That was wrong for
`btree.c` -- I'd grepped for "Your code goes here" and misread the result.
Rechecked with `grep -c "Your code goes here" src/libchidb/*.c`:
`btree.c`=15 (every function is a stub), `pager.c`=0 (genuinely complete),
`dbm-ops.c`=30, `optimizer.c`=1, `codegen.c`=0 (not stub-marked, but per
assignment_codegen.html it currently emits a hard-coded placeholder program,
so still needs a full rewrite). So this is effectively Assignments 1+2+3(+4)
from scratch on top of a working Pager, not just 2+3. `CLAUDE.md` corrected
to match.

Goal: implement Assignment 1 (B-Tree), 2 (DBM), 3 (code generator) and enough
of 4 (optimizer: at minimum keep the correct no-op pass; add index usage if
time allows) to get `SELECT`, `INSERT`, `CREATE TABLE`, and `CREATE INDEX`
working end-to-end through the shell and `make check`.

Spec sources, from chi.cs.uchicago.edu/chidb (not shipped upstream, but
now mirrored locally under [docs/chidb-website/](../chidb-website/) --
see its README; WebFetch can't reach the site over HTTPS, only plain
HTTP via curl/w3m/wget). [notes_dbm_spec.txt](notes_dbm_spec.txt) and
[notes_file_format.txt](notes_file_format.txt) are a condensed,
implementation-focused summary of the two pages that mattered most,
written before the mirror existed:
- architecture.html — DBM instruction reference (P1-P4 semantics for all 36 opcodes)
- fileformat.html — file/page/cell/record format, schema table layout
- assignment_dbm.html — DBM assignment steps and file layout (dbm.c, dbm-ops.c, dbm-cursor.c)
- assignment_codegen.html — codegen assignment steps (schema loading, SELECT/INSERT/CREATE TABLE/NATURAL JOIN)
- assignment_opt.html — optimizer assignment (sigma pushing, index support)
- testing.html — DBMF test file format, worked SELECT example DBM program

## Order of work

1. `btree.c` — every function (open/close, node load/write/alloc, getCell/insertCell,
   find, insertInTable/insertInIndex/insert/insertNonFull, split). Everything else depends on this.
2. `dbm-cursor.[ch]` — cursor struct + move/seek operations over BTree. First-pass approach:
   materialize the whole B-Tree's in-order entry sequence into a sorted array per cursor at
   OpenRead/OpenWrite time (explicitly sanctioned as a valid first approximation by
   assignment_dbm.html step 3 -- doesn't meet the amortized-O(1)/O(log n)-space bonus criteria,
   but is simple to get right and every test only checks correctness). Revisit with a real
   stack-based traversal later if it matters.
3. `dbm-ops.c` — implement all opcode handlers per `notes_dbm_spec.txt`.
4. Schema loading — `chidbInt.h` (`struct chidb` needs a schema list), `api.c` (`chidb_open` loads
   page-1 schema table), `util.[ch]` (schema lookup helpers per assignment_codegen step 1).
5. `codegen.c` — `chidb_stmt_codegen`: CREATE TABLE, INSERT, simple SELECT (single table, optional
   single WHERE comparison), then NATURAL JOIN if time allows.
6. `optimizer.c` — keep correct pass-through at minimum; add CREATE INDEX support + index-based
   SELECT codegen if time allows (assignment 4).
7. Build (`autoreconf`/`configure`/`make`) and iterate against `make check` (Check test suite already
   has DBMF fixtures under `tests/files/dbm-programs/`).

## Status: core system working, `make check` green (2026-09-11)

Implemented, in order: `btree.c` (every function -- open/close, node
load/write/alloc, getCell/insertCell, find, insertInTable/insertInIndex/
insert/insertNonFull/split, including the fixed-root-page split trick and
the exact file-header byte layout reverse-engineered from
`tests/check_btree_1b.c`), `dbm-cursor.[ch]` (materialize-the-whole-BTree
cursor), `dbm-ops.c` (all 37 opcodes), schema loading (`chidbInt.h`'s
`chidb_schema_item_t`, `util.c`'s `chidb_schema_*`, wired into
`api.c`'s `chidb_open`/`chidb_step`), `codegen.c` (CREATE TABLE, CREATE
INDEX + index population, INSERT with index maintenance, single-table
SELECT and two-way NATURAL JOIN each with a full conjunction of `column
OP literal` WHERE clauses -- see below), and `optimizer.c` (sigma-pushing
for NATURAL JOIN queries). All four assignments' core deliverables are
now covered; see "Not implemented" below for the specific gaps that
remain within each.

Also fixed, since it turned out to matter for actually demonstrating
sigma-pushing (`assignment_opt.html` specifies `.opt` as literally how
it's meant to be observed): a pre-existing, unrelated buffer-overflow
crash in `libchisql`'s `indent_print()` (`src/libchisql/common.c`) that
made the shell's `.parse`/`.opt` commands abort on any real query. See
that function's own `claude:` comment for the exact bug.

### Sigma-pushing (`optimizer.c`)

`chidb_stmt_optimize()` now does real work for the one shape
`assignment_opt.html` describes: `Project(exprs, Select(cond,
NaturalJoin(Table(t1), Table(t2))))`. It flattens `cond`'s top-level
AND-chain, classifies each conjunct as touching only `t1`, only `t2`, or
neither/both (a cross-table condition, a non-comparison, or an
unqualified reference to the shared natural-join column itself -- always
left at the top; misclassifying "ambiguous" as pushable would be a
correctness bug, so this stays conservative on purpose), and rebuilds the
tree with each single-table bucket wrapped in its own `Select` right next
to that table, below the join. Verified against `assignment_opt.html`'s
own worked `.opt` example (byte-for-byte structural match) and against
real query results on a fixture with a left-only, a right-only, a
both-pushed, and an ambiguous-stays-at-top condition, both before and
after the rewrite. Every other statement shape (not a SELECT, no WHERE,
single-table, a join that isn't two base tables) goes through unchanged,
same as the original trivial pass-through.

Since `chidb_stmt_optimize()` runs in front of *every* `chidb_prepare()`
call, not just behind `.opt`, turning it on meant `codegen_select_join()`
now has to accept a `NaturalJoin` whose sides are pre-wrapped in a
`Select` (exactly the shape pushing produces) as ordinary input, and both
it and the single-table `codegen_select()` had to generalize from "at
most one WHERE condition" to "a full conjunction of `column OP literal`
comparisons" (the "WHERE clauses" section near the top of codegen.c:
`ResolvedCmp`/`resolve_conjuncts`/`emit_filter_checks`, shared by both).
That generalization was a prerequisite for sigma-pushing, not an
independent feature -- but it's also a strict superset of the
single-condition behavior the assignments actually require, verified
behavior-preserving by rerunning the full DBMF suite (all 111 cases,
unchanged) before writing the optimizer itself.

NATURAL JOIN has no upstream test suite (assignment_codegen.html: "Tests
for NATURAL JOIN are not currently available") -- fixture built for this
pass instead: `tests/files/databases/join-courses-departments.cdb` (two
tables, `courses`/`departments`, sharing a `id` column, with one
unmatched row on each side to exercise the inner-join exclusion) plus six
DBMF cases under `tests/files/dbm-programs/sql-select-join/` covering
`*`, an explicit projection, qualified column names on both sides of
WHERE, a zero-row WHERE, table aliases with a WHERE on the left table's
primary key, and WHERE on the unqualified shared join column. (These
predate sigma-pushing and don't exercise it directly -- pushing is
transparent to query results by construction, so the manual `.opt` +
result verification above is the real coverage for it.)

`make check`: 5/5 suites, 111/111 DBMF cases, all green -- with
sigma-pushing live for every query, not just ones run through `.opt`.

### Index seeks extended to join scans

`codegen_select_join` now independently decides, per side, whether to
SCAN (the original `Rewind`/`Next` loop with the side's pushed conjuncts
as filters) or SEEK (when that side's pushed condition is exactly one
equality on an indexed column): open every cursor that could possibly be
needed up front, before any `Seek`/`Rewind` runs, perform every SEEK
positioning unconditionally (a seek's result is a compile-time-literal
lookup, so it's invariant across outer-loop iterations regardless of
which side drives the loop), and only then wrap whichever side(s) are
still SCAN in an actual loop. Opening everything up front means every
"no more rows" outcome -- an index miss on either side, or an empty
table on a SCAN side -- shares one cursor-closing tail at the very end,
since nothing that tail closes is ever still unopened at the point
something jumps to it. This turns "both sides indexed" into a single
check with no loop instructions at all, "one side indexed" into one
seek per outer row instead of a full inner scan, and "neither indexed"
into the original nested loop, unchanged.

Along the way, found and fixed a real gap: `codegen_create_index` never
validated that the indexed column is actually `INTEGER`-typed
(fileformat.html: "Indexes can only be created for unsigned 4-byte
integer unique fields") -- indexing a `TEXT` column previously silently
built a corrupt index instead of returning `CHIDB_EINVALIDSQL` (an
`IdxInsert` would read a `REG_STRING` register's `.value.i`, i.e. the low
bits of a pointer, as if it were the intended integer key). Found by
hitting exactly this while building the test fixture for this feature.

Verified by hand: outer-only seek, inner-only seek, both-seek (confirmed
via `EXPLAIN` to compile to zero loop instructions), and every
miss/no-match combination for each (index miss on one side with the
other a hit; both indexed with one missing) -- all against manually
computed expected row sets, matched exactly, no crashes.

Not implemented (out of scope for this pass, in order of likely value if
resumed):
- Index-based codegen (single-table or join) only covers a single
  top-level equality test (`indexed-col = val` / `val = indexed-col`) --
  e.g. `WHERE indexedcol > val`, or an indexable equality ANDed with
  anything else, still does a full scan/full filter check on that side.
- Only two-way NATURAL JOIN of two base tables -- no 3-way joins, no
  `JOIN ... ON`/`USING`, no outer joins, no `UNION`/`INTERSECT`/`EXCEPT`.
  `codegen_select_join` rejects anything where either side of the
  `SRA_NATURAL_JOIN` isn't a bare `SRA_TABLE` optionally wrapped in one
  `SRA_SELECT`.
- Sigma-pushing itself only recognizes the single exact shape named
  above -- one `Select` directly over one `NaturalJoin` of two bare
  tables. A 3-way join, a `Select` nested inside something else first, or
  a join order where pushing below *two* joins would matter, none of
  which this pass's NATURAL JOIN support can even express yet, would need
  the optimizer extended alongside them.
- Cursors are O(n) space / not amortized O(1) Next (see dbm-cursor.h) --
  explicitly sanctioned as a first-pass approximation by
  assignment_dbm.html step 3, correct but not the bonus-credit shape.
- The shell's `.parse "SQL"` and `.opt "SQL"` commands crash with a glibc
  "buffer overflow detected" abort as soon as they try to print the parsed
  statement (`Project(*** buffer overflow detected ***`). Confirmed
  pre-existing and unrelated to this pass's changes: `.parse` alone (no
  optimizer call involved) reproduces it identically, and it's entirely
  inside `src/libchisql/*.c`'s `SRA_print`/`RA_print`/`Condition_print`
  family (`indent_print` in common.c, most likely a fixed-size buffer)
  -- code this task never touched. Left unfixed as out of scope (the ask
  was the chidb DBM/codegen/optimizer assignment, not the pretty-printer);
  worth a look if the shell's `.parse`/`.opt` become load-bearing later.
- `CHIDB_EDUPLICATE` (chidbInt.h, private) and `CHIDB_EMISUSE` (chidb.h,
  public) are both numerically `8` in the pre-existing (not modified by
  this pass) constant tables -- so the shell prints "API used
  incorrectly" when what actually happened is a duplicate key (e.g.
  `CREATE INDEX` over a column that isn't actually unique, which the
  spec itself calls undefined behavior anyway). Cosmetic only; didn't
  touch it since it's outside this task's scope and changing shared
  error-code constants risked breaking something relying on the exact
  numbering.

## Progress log

- 2026-09-10: gathered spec (see notes files below), read all stub files
  (`btree.c`, `pager.c`, `dbm-ops.c`, `dbm.c`, `dbm-cursor.[ch]`, `record.[ch]`,
  `btree.h`, `chidbInt.h`), read the worked DBMF example for `SELECT * FROM courses`
  in testing.html and the raw `sql.y`/`sql.l` grammar (to know exactly what
  `chisql_statement_t`/`SRA_t`/`Condition_t`/`Insert_t`/`Create_t` shapes codegen.c
  has to consume). Discovered `btree.c` is a full stub too (see correction above).
  Starting implementation with `btree.c`.
