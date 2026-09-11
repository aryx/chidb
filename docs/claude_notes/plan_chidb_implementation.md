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

Spec sources (not in the repo, fetched from chi.cs.uchicago.edu/chidb —
notes on relevant details saved in [notes_dbm_spec.txt](notes_dbm_spec.txt)
and [notes_file_format.txt](notes_file_format.txt) since those pages
aren't stored locally and WebFetch can't reach the site over HTTPS,
only plain HTTP via curl/w3m):
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
`api.c`'s `chidb_open`/`chidb_step`), and `codegen.c` (CREATE TABLE,
CREATE INDEX + index population, INSERT with index maintenance,
single-table SELECT with an optional single `column OP literal` WHERE
compiled to an index seek whenever it's an equality test on an indexed
column -- all of assignment_opt.html's "Supporting Indexes" section --
and two-way NATURAL JOIN with qualified/unqualified column names,
assignment_codegen.html step 5). `optimizer.c` left as the original
correct no-op pass-through (still safe: codegen never retains the
`sql_stmt` pointer past the call, so the shallow-copy-then-free in
`chidb_prepare` doesn't dangle).

NATURAL JOIN has no upstream test suite (assignment_codegen.html: "Tests
for NATURAL JOIN are not currently available") -- fixture built for this
pass instead: `tests/files/databases/join-courses-departments.cdb` (two
tables, `courses`/`departments`, sharing a `id` column, with one
unmatched row on each side to exercise the inner-join exclusion) plus six
DBMF cases under `tests/files/dbm-programs/sql-select-join/` covering
`*`, an explicit projection, qualified column names on both sides of
WHERE, a zero-row WHERE, table aliases with a WHERE on the left table's
primary key, and WHERE on the unqualified shared join column.

`make check`: 5/5 suites, 105/105 DBMF cases, all green. Manually verified
past that: CREATE TABLE / INSERT / SELECT (with and without WHERE) /
CREATE INDEX + indexed lookup all work through the `chidb` shell on a
freshly created file (see `demos/`).

Not implemented (out of scope for this pass, in order of likely value if
resumed):
- Sigma-pushing (assignment_opt.html's other optimization: rewriting
  `Select(cond, NaturalJoin(t1,t2))` into `NaturalJoin(Select(cond,t1),
  t2)`). Doesn't matter for *correctness* here -- `codegen_select_join`
  already evaluates the WHERE inside the join's inner loop, so a
  selective WHERE on one side isn't paying to materialize the whole join
  first regardless. What it would still buy: replacing that side's full
  Rewind/Next scan with an index seek (see the next point) before even
  entering the loop. Nothing currently does that.
- Index-based codegen only covers the single-table case
  (`codegen_select_indexed`) and only a top-level equality test
  (`indexed-col = val` / `val = indexed-col`) -- e.g. `WHERE indexedcol >
  val` still does a full scan, joins never use an index for either side,
  and only one WHERE clause is ever supported at all (single-table *or*
  joined), so an indexable equality ANDed with something else won't use
  the index either.
- Only two-way NATURAL JOIN of two base tables -- no 3-way joins, no
  `JOIN ... ON`/`USING`, no outer joins, no `UNION`/`INTERSECT`/`EXCEPT`.
  `codegen_select_join` rejects anything where either side of the
  `SRA_NATURAL_JOIN` isn't itself a bare `SRA_TABLE`.
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
