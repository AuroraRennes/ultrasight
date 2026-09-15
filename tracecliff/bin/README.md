# bin/

Compiled benchmark binaries produced by `make` (see the top-level `Makefile` and `README.md`).

| name pattern | directory | description |
|---|---|---|
| `bench_n<N>_s<S>_i<ITERS>` | `addr_loop/` | `bench_addr.c`, loop mode |
| `bench_chain_n<N>_s<S>_i<ITERS>` | `addr_chain/` | `bench_addr.c`, chain mode, built from the generated stubs in `../chains/` |
