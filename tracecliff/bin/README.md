# bin/

Compiled benchmark binaries produced by `make` (see the top-level `Makefile` and `README.md`).

| name pattern | directory | description |
|---|---|---|
| `bench_n<N>_s<S>_i<ITERS>` | `addr_loop/` | `bench_addr.c`, loop mode |
| `bench_chain_n<N>_s<S>_i<ITERS>` | `addr_chain/` | `bench_addr.c`, chain mode, built from the generated stubs in `../chains/` |
| `bench_call_k<CALL_K>_l<CALL_LEN>_i<ITERS>` | `call/` | `bench_call.c`, dynamically linked so the callee lands outside the traced range |
| `bench_call_fill_k<CALL_K>_i<ITERS>` | `call_fill/` | `bench_call.c` at `CALL_LEN=1` with `CALL_FILL` branch-free adds per iteration |
