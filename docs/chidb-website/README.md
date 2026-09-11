# chidb website mirror

A local, offline mirror of the chidb assignment documentation from
<http://chi.cs.uchicago.edu/chidb/index.html> (not shipped in this repo upstream, and not reachable
via HTTPS from this sandbox — mirrored over plain HTTP with `wget -r -p -k`, scoped to just the
`/chidb/` subsection of the wider `chi.cs.uchicago.edu` site, which also hosts unrelated sibling
projects — chirc, chiTCP, chirouter, chistributed, chisubmit — not mirrored here).

Open [`chidb/index.html`](chidb/index.html) in a browser to start reading; internal links between
these pages were rewritten to work locally, so the whole `/chidb/` section is browsable offline.
Links out to the rest of `chi.cs.uchicago.edu` (or elsewhere) still point at the live site.

Mirrored 2026-09-11. If the upstream site changes, re-run:

```sh
wget -r -l 2 -np -p -k -nH -w 1 -R "*.pdf" -e robots=off -D chi.cs.uchicago.edu \
  http://chi.cs.uchicago.edu/chidb/index.html
```
from inside this directory (`docs/chidb-website/`).

See [`../claude_notes/notes_dbm_spec.txt`](../claude_notes/notes_dbm_spec.txt) and
[`../claude_notes/notes_file_format.txt`](../claude_notes/notes_file_format.txt) for a condensed,
implementation-focused summary of the DBM opcode and file-format pages specifically (written before
this mirror existed, when re-fetching meant `curl` + `w3m -dump` each time).
