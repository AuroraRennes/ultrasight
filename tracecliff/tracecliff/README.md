# tracecliff/

The Python driver and analysis code, run from the repo root (see the top-level `README.md`) as a package: `python3 -m tracecliff <subcommand>`. `cli.py` aggregates every subcommand; none of the scripts below are meant to be run directly anymore.

| subcommand | script | description |
|---|---|---|
| `sweep-addr` | `sweep.py` | drives `cs-trace` over the built `bench_addr.c` binaries and writes CSVs into `../results/` |
| `run-all` | (in `cli.py`) | runs `sweep-addr`, sharing `--cs-trace`/`--cs-flags`/`--runs`/`--etr-list`/`--bb-list` |
