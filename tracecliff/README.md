# tracecliff

Benchmarks that stress the CoreSight ETM trace pipeline (ETM -> funnel -> ETF/ETR -> TPIU) to characterize when and why the trace path drops data, plus the Python driver that sweeps them and analyzes the results.

This repository is a part of [`ultrasight`](https://github.com/AuroraRennes/ultrasight) and traces with the superproject's `cs-trace`, found at `../cs-trace`.

## Installation

Python >=3.12, no runtime dependencies. With [uv](https://docs.astral.sh/uv/):

```bash
uv run tracecliff axes          # runs straight from the checkout
```

or install it in the environment:

```bash
pip install -e .                # provides the `tracecliff` command
python3 -m tracecliff axes      # equivalent, no install needed
```

The benchmarks cross-compile for aarch64 Linux (Zynq/ZCU104), override `CROSS` for a different toolchain prefix (default `aarch64-linux-gnu-`).

## Quickstart

A short session end to end, from a clean checkout to a report:

```bash
# 1. build. The full axes are 780 binaries (288 loop, 288 chain, 204 call);
#    one family is enough to start.
make call

# 2. sweep, on the board, as root (cs-trace needs /dev/mem).
#    --stimulus keeps it to a handful of points instead of the whole grid.
sudo python3 -m tracecliff sweep-call \
     --cs-flags "-b ZCU-104 -c 0" \
     --factor addrfilter=etm,none --runs 3

# 3. read it back. Two readers, two questions.
python3 -m tracecliff analyze              # how much of the trace was lost?
python3 -m tracecliff micro --per-branch   # what did changing that knob do?
```

Step 2 writes `results/call_<factors>_<timestamp>.csv`; both readers scan `results/*.csv` by default.

The two readers share one per-point model and one baseline, and differ in what each point is measured *against*. **`analyze`** measures it against the analytic atom count — what a lossless trace of that point must contain — and reports loss%, plus four rates per point with `--csv-out`. **`micro`** measures it against its workload's reference arm (`stall=off etr=0 bb=0 addrfilter=etm bbfilter=all`) and prints that arm's rep-to-rep spread beside it, the floor a difference has to clear to be a result.

Both carry the same guard: back-pressure relocates loss rather than removing it, so `delivered` — the share of the expected stream that arrived — is reported next to `overflow_count`. A config with `overflow_count=0` and a collapsed `delivered` has not been fixed, it moved its losses where the dependent variable cannot see them. `etr=1` delivers ~0.05 of the stream.

**Check the APU clock before trusting any run.** `cpufreq` is not a witness — `scaling_cur_freq` reports 1199999 while the core runs at 598 MHz, because the clock driver does not apply APLL's DIV2 bit. `cpuinfo_cur_freq` is root-only and honest. Every offered rate scales with the clock.

## CoreSight ETM

The ETM reports control-flow by chaining `ATOM` and `ADDR` packets. `ATOM` packets show if the branch has been taken (`E`) or not (`N`) and address packets notify where the program jumped. By default, some `ATOM` packets may remove some `ADDRESS` packets that can be infered from the disassembly, *i.e.* with `ADDR1 ATOM(NNENE) ADDR2`, we need access to the binary to know where that middle `E` jumped too.

Branch-broadcasting (if available), guarantees that **every** `E` atom gets an address packet after, effectively removing the need for binary disassembly at the cost of a lot more trace. an `ATOM` packet is one byte, where an `ADDRESS` packet is from 1 to 9 bytes. CoreSight uses a three-slot cache of previously encountered address and can encode a new address by either matching directly to one cache slot or changing part of it (lower bits).

At high frequencies, the mismatch between the execution speed of the program and the CoreSight encoding can cause the CoreSight packets FIFO to drown and output `OVERFLOW` packets. This repo objective is to qualify them.

## The benchmarks

Two stimulus families, each stressing a different part of the pipeline.

- **`addr` sets cost and rate.** Loop and chain modes span the offered-byte-rate range the overflow knee sits in, chain reaching it with the denser packet stream.
- **`call` crosses the text boundary.** The addr families are hermetic — their stubs are in the same segment, so the loop never leaves `map_info[0]`. `--addrfilter` and `--bbfilter` are *defined* relative to that boundary and unmeasurable without traffic across it.

### 1. `src/bench_addr.c` — indirect call / address-packet stress

A loop of indirect calls (`table[i % N_TARGETS]()`) through noinline stub functions.

| knob | values | effect |
|---|---|---|
| `N_TARGETS` | 1 … 8, 16 | unique call targets. The ETM has a 3-slot exact-match address cache. |
| `STUB_STRIDE` | 0, 128, 256, 512 | byte spacing between stubs (0 = compiler packing). Sets whether the ETM needs a cheap Short address packet or an expensive Long (9-byte) one. |
| `ITERS` | 10^2 … 10^9 | iteration count. |

**Chain mode** (`-DCHAIN`, from assembly generated by `gen-chain`) replaces the loop counter and table load: each stub does one unconditional indirect `br` to the next, with a single `ret` at the end of the lap. Address packets are not diluted by loop instructions.

`N_TARGETS` is deliberately *not* a clean rate dial — it moves packet cost and execution speed at once. Few targets means a hot I-cache and BTB, so the cheap-packet configuration is also the fast one.

### 2. `src/bench_call.c` — range-crossing / TRACE_ON stress

A hot loop carrying one period-64 conditional branch per iteration, with a PLT call into libc (`strlen` on a `CALL_LEN`-byte string) on `CALL_K` of every `CALL_P` = 16 iterations.

| knob | values | effect |
|---|---|---|
| `CALL_K` | 0 … 16 | calling iterations out of every 16, spread evenly: the crossing density, linear in steps of 1/16. `0` never calls: same loop, no crossing, the control point. The pattern is a mask read at run time, so the code is identical for every value. |
| `CALL_LEN` | 1, 64, 256, 1024 | bytes `strlen` scans per call, how long each call stays in libc. `1` is the bare crossing; longer strings run `strlen`'s loop, whose taken branches branch broadcast pays for outside the text. |
| `ITERS` | 10^6, 10^7, 10^8 | iteration count. |

Two build choices are load-bearing: the binary is **dynamically linked** (no `-static`), so libc is a mapping the ETM range does not cover, and it is built **`-fno-builtin`**, so `strlen` stays a real PLT call instead of being expanded inline. The callee is very simple (and fast!) on purpose, so what is measured is the crossing, not the library's work.

### Building

```bash
make              # everything: addr_loop, addr_chain, call
make addr_loop    # bench_addr.c loop mode  -> bin/addr_loop/
make addr_chain   # bench_addr.c chain mode -> bin/addr_chain/
make call         # bench_call.c            -> bin/call/
make dump N=64 S=4096 I=1000   # objdump one chain binary
make clean
```

Binaries are named `bench_n<N>_s<S>_i<ITERS>` (loop), `bench_chain_n<N>_s<S>_i<ITERS>` (chain) and `bench_call_k<CALL_K>_l<CALL_LEN>_i<ITERS>` (call).

The swept values are not written in the Makefile: it includes `axes.mk`, generated from `tracecliff/benches.py` — the same declaration the sweeps iterate over, so the build and the sweep cannot drift apart. `make` regenerates it on its own.

### Disassembly

**`addr` loop** (`bin/addr_loop/bench_n4_s4096_i100000`) — unrolled 8x; each call site stores the index through a volatile sink, reloads it, loads the target and does a `blr`:

```asm
   41034:	str	wzr, [x19]              // idx_sink = 0
   41038:	ldr	w0, [x19]
   4103c:	ldr	x0, [x20, w0, uxtw #3]  // table[idx_sink]
   41040:	blr	x0                      // indirect call -> addr packet
   ...                                    // 7 more unrolled call sites
   410b4:	subs	w21, w21, #0x1
   410b8:	b.ne	41034 <bench+0x30>
```

Each stub is one `ret` padded out to `STUB_STRIDE`:

```asm
0000000000002000 <t0>:
    2000:	ret                             // return -> second addr packet
    2004:	nop                             // ... padding up to t1 at 0x3000
```

**`addr` chain** (`chains/chain_n4_s4096.S`) — one call to `t0` runs 4 hops and returns once:

```asm
	.balign 4096
t0:	adr	x11, t1
	br	x11         // indirect branch -> addr packet, no stack, no memory traffic
	.balign 4096
t1:	adr	x11, t2
	br	x11
	...
t3:	ret             // single return at the far end of the lap
```

**`call`** (`bin/call/bench_call_k5_l1_i10000000`) — x19 counts iterations, w22 holds the call mask read once from a volatile, x20 holds `ITERS`:

```asm
 864:	b	878 <bench+0x48>
 868:	bl	82c <skip_branch>         // period-64 not-taken path
 86c:	add	x19, x19, #0x1
 870:	cmp	x19, x20                  // x20 = 0x989680 = 10,000,000 = ITERS
 874:	b.eq	8a4 <bench+0x74>
 878:	and	w0, w19, #0xf             // slot i mod CALL_P
 87c:	lsr	w0, w22, w0
 880:	tbz	w0, #0, 898 <bench+0x68>  // slot not in the mask -> skip the call
 884:	mov	x0, x23
 888:	bl	690 <strlen@plt>          // the crossing: PLT -> libc, outside the traced range
 88c:	ldr	x1, [x21, #96]
 890:	add	x0, x0, x1
 894:	str	x0, [x21, #96]
 898:	tbz	w19, #6, 868 <bench+0x38> // the period-64 branch: bit 6 of the counter
 89c:	bl	828 <take_branch>         // taken path
 8a0:	b	86c <bench+0x3c>
```

Every `CALL_K` compiles to these instructions; only the mask differs, so `CALL_K=0` is the same loop with no crossing and `CALL_K=16` crosses on every iteration.

## Sweeps

The sweep subcommands drive `cs-trace` over the built binaries, must run as root, and append one CSV row per run straight from `cs-trace`'s stats output — no stdout scraping. `cs-trace` defaults to `../cs-trace`; pass `--cs-trace` only for an out-of-tree build.

```bash
sudo python3 -m tracecliff sweep-addr --kind loop   # or --kind chain
sudo python3 -m tracecliff sweep-call
sudo python3 -m tracecliff run-all                  # all three, shared settings
```

`--stimulus GLOB` (repeatable) restricts a sweep to matching binaries — how one campaign stage runs its own points out of the full grid.

### Factors

A **factor** is a `cs-trace` knob the sweep varies. Each is simultaneously a CSV column and a flag, from one declaration, so the two cannot drift apart: `etr` (`--useetr`), `bb` (`--branchbroadcast`), `stall` (`--stall`), `addrfilter` (`--addrfilter`), `bbfilter` (`--bbfilter`).

`--factor NAME=V1,V2` sweeps one, is repeatable, and the cross product is walked.

> Factor values are **requested**, recorded as the sweep asked for them, not read back from hardware. Pass every one explicitly: an omitted flag falls back to the *invasive* level.

## Campaigns

The dependent variable is `overflow_count` (ETM Overflow packets); `raw_trace_bytes` and `child_time_s` are covariates.

The current campaign is the **overflow qualification**: three stages, each varying one part of the configuration against the shared reference arm.

| stage | factor | levels | stimulus |
|---|---|---|---|
| A | run length, then `--useetr` | `iters` ladder; etr 0 / 1 | rate ladder |
| B | `--stall` | off / 3 | comparator-cache set + 3 ladder points |
| C | `--addrfilter`, then `--bbfilter` | etm / none; all / in plus `bb=0`, at `addrfilter=none` | call |

- **A** fixes the run length every later stage uses: the shortest `iters` whose per-branch metrics sit inside the reference arm's own spread, measured in *both* stall regimes since ISTALL changes the buffer fill dynamics. It then confirms the ETR arm, already known saturated below the bottom of the ladder.
- **B** takes the N axis from the comparator-cache set and the rate axis from the ladder: a flat `child_time_s` ratio means stall is a fixed tax, a diverging one means the cost is unbounded in branch rate.
- **C** is the range-boundary stage, and the only one that needs the call family — the addr families are hermetic, so neither filter is measurable without traffic across the text boundary. Both halves read against the same x axis, crossings per second, and together they are one argument rather than two measurements:
  - **`--addrfilter`** tests whether filtering in the ETM turns every call into an untraced library into a `TRACE_ON` burst. If so, `etm` overflows at a *lower* offered rate than `none` despite tracing strictly less, and the gap between the series is the per-crossing cost. The way out of that is to stop filtering in the ETM at all: trace everything, `addrfilter=none`.
  - **`--bbfilter`** then prices that escape. Tracing all of EL0 with broadcast on everywhere is far more trace, so overflow comes back from the other side. But the analysis only needs the tracee's own control flow — the libraries are filtered downstream anyway — so `in` keeps full address detail in the tracee text and drops libc to cheap atoms. The question is whether that buys enough: `all` and `bb=0` bracket the range, and `in` should land between them in proportion to how much execution sits in the text. `in` landing on `all` means the text is where all the branches are and the filter buys nothing.
