# results/

CSVs written by `tracecliff sweep-addr`.

Naming: `<kind>_<factor tags>_<timestamp>.csv`, one tag per swept factor holding its values (e.g. `etr0-1_bb0-1`, `stalloff-3`), and a `+HHMM` UTC offset on the timestamp.

| `kind` value | produced by |
|---|---|
| `addr_loop` | `tracecliff sweep-addr --kind loop` |
| `addr_chain` | `tracecliff sweep-addr --kind chain` |

| other file | description |
|---|---|
| `*.log` | captured stdout/stderr from full sweep runs (e.g. `tracecliff run-all`), not CSV data |
| `analyzed_results/` | per-point summaries written by `tracecliff analyze --csv-out` (`analyzed_<name>.csv` per input CSV) |

Read and analyzed via `python3 -m tracecliff analyze`.
