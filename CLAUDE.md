# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

chidb is a didactic RDBMS originally developed at the University of Chicago (CMSC 23500) as a
course assignment: students are given a working SQL front end, pager, and B-Tree layer, and must
implement the database virtual machine (DBM) opcode handlers, the query optimizer, and the DBM
code generator themselves. This fork (`aryx/chidb`, remote `origin`) tracks upstream fixes via PRs
from other contributors.

**Status (2026-09-11): all four course assignments are implemented, including query
optimization** — `btree.c`, `dbm-cursor.[ch]`, `dbm-ops.c`, `codegen.c` (CREATE TABLE, CREATE INDEX
with population, INSERT with index maintenance, single-table SELECT and two-way NATURAL JOIN, each
with a full conjunction of `column OP literal` WHERE clauses), and `optimizer.c` (sigma-pushing for
NATURAL JOIN queries, verified against `assignment_opt.html`'s own worked example) all have real
implementations now. A single-table or per-join-side equality test on an indexed column compiles to
an index seek instead of a full scan — including both sides of a join independently, which for a
fully-indexed two-equality join compiles to zero loop instructions at all (see
`codegen_select_join`'s file comment). `make check` is green (5/5 suites, 111 DBMF cases) — with the
optimizer live for every query. Also fixed along the way: a pre-existing, unrelated buffer-overflow
crash in `libchisql`'s pretty-printer that broke the shell's `.parse`/`.opt` commands (see
`src/libchisql/common.c`'s `indent_print()`), and a real gap where `CREATE INDEX` on a non-integer
column silently built a corrupt index instead of erroring. See
`docs/claude_notes/plan_chidb_implementation.md` for exactly what's done vs. the remaining gaps
(equality-only index use, 3-way joins, and a few others), `changes.txt` for the project's full
history back to its 2009 origin, and `docs/claude_notes/notes_*.txt` for the file-format/DBM-opcode
spec pulled from chi.cs.uchicago.edu/chidb (not shipped in this repo, and not reachable via HTTPS
from this sandbox — fetched over plain HTTP with `curl` + `w3m -dump`). Small runnable examples are
under `demos/`.
Before assuming a SQL feature works or doesn't, check the plan file's "Not implemented" list first.

## Build

Autotools project (no CMake/Meson). Requires `flex`/`bison`, `libedit` (+ `histedit.h`), and
optionally `check` (>= 0.9.14) for the test suite.

```sh
./autogen.sh          # only needed if configure/Makefile.in are missing (regenerates via autoreconf)
./configure
make                  # builds libchidb.la, libchisql.la, libsimclist.la, and the `chidb` shell binary
```

## Tests

Tests use the [Check](https://libcheck.github.io/check/) C unit testing framework, wired into
`make check` via `TESTS` in `Makefile.am`.

```sh
make check                                  # build and run all test binaries
./tests/check_btree                         # run one test binary directly (from build dir)
CK_RUN_CASE="..." ./tests/check_btree        # Check env vars to isolate a single case
CK_RUN_SUITE="..." ./tests/check_btree
```

Test binaries: `check_btree` (split across `check_btree_1a.c`...`check_btree_8.c` + shared
`check_btree_common.c`/`check_btree_files.c`), `check_dbrecord`, `check_dbm`, `check_pager`,
`check_utils`. Fixture files live under `tests/files/{databases,dbm-programs,generated}/`; test
code resolves them via `TEST_DIR` (set to `$(srcdir)/tests/` by the Makefile), not relative paths,
so tests are runnable from the build directory.

## Architecture

chidb mirrors SQLite's layered design. Data flows top-to-bottom through these layers, each only
aware of the one below it:

```
SQL text
  │  src/libchisql: sql.l/sql.y (flex/bison) → chisql AST
  ▼
chisql AST          (include/chisql/*.h: create, insert, delete, condition, expression, column, literal)
  │  ra.c / sra.h describe the RA/SRA algebra the parser output maps onto
  │  (SQL → SRA → RA, see comment in include/chisql/sra.h)
  ▼
optimizer.c          chidb_stmt_optimize(): sigma-pushing for `Select(cond, NaturalJoin(t1,t2))`
                     queries (pushes single-table conjuncts down next to their table); a no-op
                     pass-through for every other statement shape
  ▼
codegen.c             chidb_stmt_codegen(): compiles the (optimized) statement into DBM bytecode
  ▼
DBM bytecode          array of chidb_dbm_op_t (opcode + 3 int args + string arg), see dbm-types.h
  │  src/libchidb/dbm.c: chidb_stmt_exec() drives the fetch-decode-execute loop
  │  src/libchidb/dbm-ops.c: one function per opcode (Noop, OpenRead, Next, Column, Eq, Insert, ...)
  │  src/libchidb/dbm-cursor.c: table/index cursors used by opcodes like Next/Seek* — first-pass
  │    implementation that materializes a whole B-Tree's in-order sequence into a sorted array per
  │    cursor rather than a stack-based O(log n)-space traversal (see dbm-cursor.h)
  ▼
BTree                 src/libchidb/btree.c: table/index B-Trees over pages (chidb_Btree_find/insert/split/...)
  ▼
Pager                 src/libchidb/pager.c: page-level file I/O (chidb_Pager_readPage/writePage/allocatePage)
  ▼
chidb file on disk
```

Public API (`include/chidb/chidb.h`: `chidb_open`/`chidb_prepare`/`chidb_step`/`chidb_finalize`/
`chidb_column_*`/`chidb_close`) is implemented in `src/libchidb/api.c` and is a thin sqlite3-style
prepare/step/finalize wrapper around the DBM. `struct chidb` (in `chidbInt.h`) wraps a `BTree *`
plus an in-memory `chidb_schema_item_t` list (loaded from the page-1 schema table by
`chidb_schema_load()` in `util.c`, on `chidb_open()` and again after every `CREATE TABLE`/`CREATE
INDEX` — see `chidb_stmt.schema_change` in `dbm-types.h`); `codegen.c` and `optimizer.c` use
`chidb_schema_find_table()`/`chidb_schema_find_index_on()`/`chidb_schema_next_key()` to resolve
table/column names and root pages instead of re-walking the B-Tree.

`src/libchidb/dbm-file.c` implements a `chidb_dbm_file_t` used by the test suite (and possibly
tooling) to run a `.dbm` program file directly against a database without going through the SQL
front end — useful for testing DBM opcodes in isolation from the parser/codegen (see
`tests/files/dbm-programs/`).

`src/simclist/` is a vendored third-party generic C list library (not chidb code); avoid modifying
it beyond what's needed to keep it building against project CFLAGS.

`src/shell/` is the interactive `chidb` REPL binary (uses `libedit`/`histedit.h`), built on top of
`libchidb`. Not a layer other code depends on.

## Demos

`demos/` has small, runnable examples exercising the shell end-to-end (CREATE TABLE, INSERT, SELECT
with/without WHERE, CREATE INDEX) — see `demos/README.md`.

### Error codes

Two overlapping sets in `include/chidb/chidb.h` (public, `CHIDB_*`) and `chidbInt.h` (internal-only
extras like `CHIDB_ECELLNO`, `CHIDB_ENOTFOUND`, `CHIDB_ECORRUPTHEADER`). `CHIDB_OK` is `0`; most
functions return one of these codes rather than using `errno`/exceptions.
