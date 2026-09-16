# tracecliff/

The Python driver and analysis code, run from the repo root (see the top-level `README.md`) as a package: `python3 -m tracecliff <subcommand>`. `cli.py` aggregates every subcommand; none of the scripts below are meant to be run directly anymore.

| subcommand | script | description |
|---|---|---|
| `sweep-addr` | `sweep.py` + `benches.py` | drives `cs-trace` over the built `bench_addr.c` binaries (`--kind loop\|chain`) and writes CSVs into `../results/` |
| `sweep-call` | `sweep.py` + `benches.py` | drives `cs-trace` over the built `bench_call.c` binaries and writes CSVs into `../results/` |
| `axes` | `benches.py` | prints the sweep axes; `--make` emits them as Make variables (see below) |
| `analyze` | `points.py` + `analyzer.py` + `reporter.py` | reads CSVs back from `../results/` and produces the text report and `--csv-out` summaries |
| `gen-chain` | `gen_chain.py` | generates the chain-mode stub assembly in `../chains/`, invoked automatically by the Makefile |
| `run-all` | (in `cli.py`) | runs `sweep-addr` (loop, chain) then `sweep-call` sequentially, sharing `--cs-trace`/`--cs-flags`/`--runs` and the swept factors |

The sweeps are one engine and a declaration per family, split across two modules that are not subcommands themselves:

- **`sweep.py`, the engine.** Everything the sweeps share: the `cs-trace` invocation and the columns kept from its CSV row, the loops over the swept factors (`--factor NAME=V1,V2`, `--etr-list`, `--bb-list`), the per-point repetition, and the output file.
- **`benches.py`, what to iterate over.** A sweep is a `Bench`: an ordered tuple of `Axis`es (CSV column, print label, values, Make variable) plus a rule mapping a point to a binary name. `addr_bench(kind)` builds one per `--kind` and `CALL_BENCH` is the call one, along with the atom count each point should decode, which depends on the point and on `addrfilter`.

Adding a benchmark family therefore means adding a `Bench` there and a subparser in `cli.py`, not copying the engine.

### axes.mk, one place for the axis values

The Makefile does not repeat the axis lists. It includes `axes.mk`, generated from `benches.py`:

```make
axes.mk: tracecliff/benches.py
	python3 -m tracecliff axes --make > $@
include axes.mk
```

Make regenerates an included file and restarts itself automatically, so plain `make` still works and `axes.mk` is gitignored. The binaries the Makefile builds and the points the sweeps walk come from the same declaration, and adding a value to an axis in `benches.py` is the whole change.
