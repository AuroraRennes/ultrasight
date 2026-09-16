# src/

The benchmarks, cross-compiled by the top-level `Makefile` (see the repo root `README.md`):

| file | description |
|---|---|
| `bench_addr.c` | stresses the indirect-call / address-packet path |
| `bench_call.c` | calls out of the traced range into libc, one TRACE_ON burst per crossing |
