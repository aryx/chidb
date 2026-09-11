# chidb demos

Small, runnable examples exercising the shell end-to-end, once the project is built (see the
repository root `CLAUDE.md` for build instructions):

```sh
./configure && make
./chidb /tmp/library.cdb < demos/library.sql
```

- `library.sql` — creates a small `books` table, inserts a few rows, runs `SELECT` with and
  without a `WHERE` clause, and creates an index.
- `session.txt` — the real output of running `library.sql` above (plus a couple of interactive
  follow-up queries against the resulting file), with commentary. Not a mock-up.

Known gaps that don't show up in this demo (see
[`../docs/claude_notes/plan_chidb_implementation.md`](../docs/claude_notes/plan_chidb_implementation.md)
for the full picture): no `NATURAL JOIN`, and only a single top-level `WHERE indexedcol = val` (or
`val = indexedcol`) equality test compiles to an index seek — anything else (`>`, an indexed column
ANDed with another condition, etc.) still falls back to a full table scan in primary-key order.
