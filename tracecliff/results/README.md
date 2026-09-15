# results/

CSVs written by `tracecliff sweep-addr`.

Naming: `<kind>_etr<E>-<E>[_bb<B>-<B>]_<timestamp>.csv`, the `bb` chunk is only present for sweeps that also swept `--bb-list`.

| `kind` value | produced by |
|---|---|
| `addr_loop` | `tracecliff sweep-addr --kind loop` |
| `addr_chain` | `tracecliff sweep-addr --kind chain` |

| other file | description |
|---|---|
| `*.log` | captured stdout/stderr from full sweep runs (e.g. `tracecliff run-all`), not CSV data |
| `analyzed_results/` | per-point summaries written by `tracecliff analyze --csv-out` (`analyzed_<name>.csv` per input CSV) |

Read and analyzed via `python3 -m tracecliff analyze`.
