# plots/

Figures written by `python3 -m tracecliff plot` (see `tracecliff/plotter.py`'s docstring), run from the repo root. This directory is the default output. Name a figure to draw only that one: `plot cliff corpus`.

Everything is drawn at `bb=1`. Port load is trace bytes per second over the 1000 MB/s the TPIU port can carry (32 bits at 250 MHz). A cell's offered load is the byte count of its overflow-free runs over its `stall=off` run time. A sink's ceiling is the median load it delivers at `stall=off` on the cells where every run overflowed.

| name | question | campaign |
|---|---|---|
| `cliff.png` | is loss a function of offered rate alone, and what does `stall=3` cost? | `sweep_addr.sh` (chain and loop binaries) and `call_fill.sh`: synthetic benches, both sinks, `stall` off and 3 |
| `filters.png` | what does an address-filter crossing cost in trace bytes? | `call_fill.sh`: a loop where `call_k` of every 16 iterations call into libc, `addrfilter` etm and none |
| `corpus.png` | which fuzzing inputs overflow, under which filter, and does `stall=3` remove it? | `targets_corpus.sh`: real targets replayed on the board over the minimized corpus of a 24 h QEMU fuzzing run (staged by `stage_corpus.sh`), three filter arms, `stall` off and 3 |
