# chidb demos

Small, runnable examples exercising the shell end-to-end, once the project is built (see the
repository root `CLAUDE.md` for build instructions):

```sh
./configure && make
./chidb /tmp/library.cdb < demos/library.sql
```

- `library.sql` — creates a small `books` table, inserts a few rows, runs `SELECT` with and
  without a `WHERE` clause, creates an index, and then runs an equality `WHERE` and a `WHERE year >
  1985` range query that both end up using it.
- `join.sql` — two tables sharing a column name, joined with `NATURAL JOIN`, including a table
  alias and a table-qualified `WHERE`.
- `sigma-push.sql` — a `NATURAL JOIN` with a single-table, indexed, range `WHERE`, shown through
  `.opt` both before and after the query optimizer pushes that condition down next to its own
  table — where it then compiles to an index range seek on that side of the join.
- `session.txt` — the real output of running all three scripts above (plus a couple of interactive
  follow-up queries against the resulting file), with commentary. Not a mock-up.

Known gaps that don't show up in these demos (see
[`../docs/claude_notes/plan_chidb_implementation.md`](../docs/claude_notes/plan_chidb_implementation.md)
for the full picture): a `WHERE` condition compiles to an index seek only when it is a single
top-level `indexedcol OP val` (or `val OP indexedcol`) comparison — `=`, `>`, `>=`, `<`, or `<=` are
all supported now, in both single-table queries and either side of a `NATURAL JOIN`, but an indexed
comparison ANDed with anything else still falls back to a full scan with a filter check; and only
two-way `NATURAL JOIN` of base tables is supported (no `JOIN ... ON`/`USING`, outer joins, or 3-way
joins).
