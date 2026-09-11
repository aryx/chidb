# chidb demos

Small, runnable examples exercising the shell end-to-end, once the project is built (see the
repository root `CLAUDE.md` for build instructions):

```sh
./configure && make
./chidb /tmp/library.cdb < demos/library.sql
```

- `library.sql` — creates a small `books` table, inserts a few rows, runs `SELECT` with and
  without a `WHERE` clause, and creates an index (and, at the end, a `WHERE` that ends up using it).
- `join.sql` — two tables sharing a column name, joined with `NATURAL JOIN`, including a table
  alias and a table-qualified `WHERE`.
- `sigma-push.sql` — a `NATURAL JOIN` with a single-table `WHERE`, shown through `.opt` both before
  and after the query optimizer pushes that condition down next to its own table.
- `session.txt` — the real output of running all three scripts above (plus a couple of interactive
  follow-up queries against the resulting file), with commentary. Not a mock-up.

Known gaps that don't show up in these demos (see
[`../docs/claude_notes/plan_chidb_implementation.md`](../docs/claude_notes/plan_chidb_implementation.md)
for the full picture): only a single top-level `WHERE indexedcol = val` (or `val = indexedcol`)
equality test compiles to an index seek, in a single-table query — anything else (`>`, an indexed
column ANDed with another condition), and either side of a `NATURAL JOIN` even after the optimizer
pushes a condition next to it, still falls back to a full table scan; and only two-way `NATURAL
JOIN` of base tables is supported (no `JOIN ... ON`/`USING`, outer joins, or 3-way joins).
