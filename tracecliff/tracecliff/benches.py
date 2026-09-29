"""
benches.py — benchmark families for sweep to iterate over

Single source of truth for the sweep axes, read by cli.py (which hands a
`Bench` to sweep.py) and by the Makefile through a generated axes.mk, produced
from the `make_var` names below with:

    python3 -m tracecliff axes --make > axes.mk

Adding a benchmark family means adding a Bench here and a subparser in cli.py.
Adding a new value to an axis here builds the new binaries and sweeps them.
"""

from tracecliff.exception import AxisConflictError
from tracecliff.sweep import Axis, Bench

# ---------------------------------------------------------------------------
# addr — indirect call / address-packet stress (src/bench_addr.c)
# ---------------------------------------------------------------------------

# N_TARGETS: 1 target = baseline (same address every call, best compression);
#            3+ targets is past the ETM's 3-slot exact-match address history,
#            forcing a real address packet on every call.
# 5, 6 and 7 fill the knee's rate hole (nothing sampled between 570 and
# 746 MB/s), their predictor behaviour lying between N=4's and N=8's.
ADDR_N_LIST = [1, 2, 3, 4, 5, 6, 7, 8, 16]
# STUB_STRIDE: byte spacing between stubs, 0 (compiler packing) or a power of two
# (.balign). It only turns short address packets into Long ones, saturated by 512.
ADDR_STRIDE_LIST = [0, 128, 256, 512]
# ITERS: run length, swept over seven decades to find the shortest run in
# steady state, with and without ETR.
ITERS_LIST = [
    100,
    1_000,
    10_000,
    100_000,
    1_000_000,
    10_000_000,
    100_000_000,
    1_000_000_000,
]

# Two build modes of the same source, differing only in binary name.
ADDR_BENCH_SUBDIR = {"loop": "bin/addr_loop", "chain": "bin/addr_chain"}
ADDR_BIN_PREFIX = {"loop": "bench", "chain": "bench_chain"}

# addrfilter values whose trace holds the tracee text alone, the scope every
# formula below counts first. None is a CSV without the column: the addr
# families never leave the text, so their count is the same.
TEXT_SCOPED_ADDRFILTERS = (None, "etm")
# With addrfilter=none the loader, libc startup and exit are traced too: a fixed
# amount on top of the text count, the mean of captured runs, which scatter by
# about +-40 around it.
UNFILTERED_ADDRFILTER = "none"

# bench_addr.c: every executed branch costs exactly 2 atom elements, so the
# analytic count is 2 x branches.
ATOMS_PER_BRANCH = 2
# Atom elements outside the bench loop (main, its printf and exit path), fixed
# per build mode: captured traces sit exactly this far above the loop count.
# TODO: confirm with more captures.
ADDR_LOOP_FIXED_ATOMS = 37
ADDR_CHAIN_FIXED_ATOMS = 33
# TODO: confirm with more captures.
ADDR_UNFILTERED_FIXED_ATOMS = 19_387
ADDR_LOOP_UNROLL = 8  # iterations per unrolled block
ADDR_LOOP_INDIRECT_PER_ITER = 2  # the indirect call and its return
# A lap of N hops is the BL to t0, N-1 BRs and the final RET (N+1 branches),
# plus the loop branch in bench() that starts the next lap.
ADDR_CHAIN_LAP_OVERHEAD_BRANCHES = 2


def addr_unfiltered_atoms(addrfilter: str | None) -> int | None:
    """Atom elements outside the tracee text, None where no formula covers them."""
    if addrfilter in TEXT_SCOPED_ADDRFILTERS:
        return 0
    if addrfilter == UNFILTERED_ADDRFILTER:
        return ADDR_UNFILTERED_FIXED_ATOMS
    return None


def addr_loop_atoms(point: tuple[int, ...], addrfilter: str | None) -> int | None:
    outside = addr_unfiltered_atoms(addrfilter)
    if outside is None:
        return None
    _n_targets, _spacing, iters = point
    # One loop-back branch per block of ADDR_LOOP_UNROLL calls, then one per
    # call in the tail loop that runs the remainder.
    blocks, tail = divmod(iters, ADDR_LOOP_UNROLL)
    branches = iters * ADDR_LOOP_INDIRECT_PER_ITER + blocks + tail
    return branches * ATOMS_PER_BRANCH + ADDR_LOOP_FIXED_ATOMS + outside


def addr_chain_atoms(point: tuple[int, ...], addrfilter: str | None) -> int | None:
    outside = addr_unfiltered_atoms(addrfilter)
    if outside is None:
        return None
    n_targets, _spacing, iters = point
    # The chain only runs whole laps of N hops: iters rounds up to one.
    laps = -(-iters // n_targets)  # ceil
    branches = laps * (n_targets + ADDR_CHAIN_LAP_OVERHEAD_BRANCHES)
    return branches * ATOMS_PER_BRANCH + ADDR_CHAIN_FIXED_ATOMS + outside


ADDR_ATOMS = {"loop": addr_loop_atoms, "chain": addr_chain_atoms}


def addr_bench(kind: str) -> Bench:
    """The addr Bench for --kind loop / chain."""
    prefix = ADDR_BIN_PREFIX[kind]
    return Bench(
        name=f"addr_{kind}",
        axes=(
            Axis("n_targets", "n", ADDR_N_LIST, make_var="ADDR_N_LIST"),
            Axis("spacing", "s", ADDR_STRIDE_LIST, make_var="ADDR_STRIDE_LIST"),
            Axis("iters", "iters", ITERS_LIST, width=10, make_var="ITERS_LIST"),
        ),
        binary_name=lambda p: f"{prefix}_n{p[0]}_s{p[1]}_i{p[2]}",
        # one printed block per N_TARGETS, i.e. the whole STUB_STRIDE x ITERS grid
        group_depth=1,
        detail=f"{kind} ({prefix})",
        expected_atoms=ADDR_ATOMS[kind],
    )


# ---------------------------------------------------------------------------
# call — range-crossing stress (src/bench_call.c)
# ---------------------------------------------------------------------------

# CALL_K: iterations out of every CALL_P that call into libc, the crossing density.
# 0 never calls (the control point), CALL_P calls on every iteration.
CALL_P = 16
CALL_K_LIST = list(range(CALL_P + 1))
# CALL_LEN: bytes strlen scans per call. 1 is the bare crossing; longer strings
# run strlen's loop, whose branches broadcast pays for outside the text.
CALL_LEN_LIST = [1, 64, 256, 1024]
# ITERS for call binaries only, independent of the addr families' ITERS_LIST.
# Multiples of CALL_P, so every point makes exactly ITERS * K / P calls.
CALL_ITERS_LIST = [1_000_000, 10_000_000, 100_000_000]

CALL_BENCH_SUBDIR = "bin/call"


def call_count(call_k: int, iters: int) -> int:
    """Library calls in one run of bench(): K of every P iterations."""
    return iters * call_k // CALL_P


# Atom elements, exact against every lossless capture. bench() runs twice,
# warmup then timed, and both are traced.
CALL_RUNS_TRACED = 2
# Per iteration of one run: the period-64 branch and its call, the mask test
# and the loop test.
CALL_BASE_ATOMS_PER_ITER = 5
# The period-64 branch's taken direction costs one atom more than the other.
CALL_TAKEN_EXTRA_ATOMS = 1
# One call: BL into the PLT and the TRACE_ON back.
CALL_ATOMS_PER_CALL = 2
# Outside the bench loop (main's memset and exit path), one more when the
# binary calls strlen at all.
CALL_FIXED_ATOMS = {False: 42, True: 43}
# A CSV without the column ran at cs-trace's default, which traces libc and the
# loader as addrfilter=none does.
LIBC_TRACED_ADDRFILTERS = (None, UNFILTERED_ADDRFILTER)
# Traced libc: the PLT stub and strlen's entry and return on every call...
CALL_UNFILTERED_ATOMS_PER_CALL = 3
# ...plus strlen's loop, which depends on CALL_LEN alone.
CALL_STRLEN_LOOP_ATOMS = {1: 0, 64: 4, 256: 10, 1024: 34}
# The loader and libc work outside the text, which binding strlen adds to: the
# mean of the addrfilter=none runs, which move by up to ~80 atoms.
CALL_UNFILTERED_FIXED_ATOMS = {False: 19_293, True: 19_895}


def call_atoms(point: tuple[int, ...], addrfilter: str | None) -> int | None:
    call_k, call_len, iters = point
    calls = call_count(call_k, iters)
    per_call = CALL_ATOMS_PER_CALL
    if addrfilter == "etm":
        outside = 0
    elif addrfilter in LIBC_TRACED_ADDRFILTERS:
        if call_len not in CALL_STRLEN_LOOP_ATOMS:
            return None
        per_call += CALL_UNFILTERED_ATOMS_PER_CALL + CALL_STRLEN_LOOP_ATOMS[call_len]
        outside = CALL_UNFILTERED_FIXED_ATOMS[call_k > 0]
    else:
        return None
    # (i >> 6) & 1 is true on the upper half of each 128-iteration cycle.
    full, rest = divmod(iters, 128)
    taken = full * 64 + max(0, rest - 64)
    per_run = (
        iters * CALL_BASE_ATOMS_PER_ITER
        + taken * CALL_TAKEN_EXTRA_ATOMS
        + calls * per_call
    )
    return (
        CALL_RUNS_TRACED * per_run + CALL_FIXED_ATOMS[call_k > 0] + outside
    )


CALL_BENCH = Bench(
    name="call",
    axes=(
        Axis("call_k", "k", CALL_K_LIST, make_var="CALL_K_LIST"),
        Axis("call_len", "l", CALL_LEN_LIST, make_var="CALL_LEN_LIST"),
        Axis("iters", "iters", CALL_ITERS_LIST, width=10, make_var="CALL_ITERS_LIST"),
    ),
    binary_name=lambda p: f"bench_call_k{p[0]}_l{p[1]}_i{p[2]}",
    # one printed block per CALL_K, i.e. its CALL_LEN x ITERS grid
    group_depth=1,
    expected_atoms=call_atoms,
)


# ---------------------------------------------------------------------------
# call_fill — bench_call at CALL_LEN=1 with a branch-free filler per iteration
# ---------------------------------------------------------------------------

# CALL_FILL: dependent adds per iteration (~0.83 ns each, no trace), placing the
# no-call control near 415 MB/s at bb=1, under both ceilings.
CALL_FILL = 24
CALL_FILL_ITERS_LIST = [10_000_000]

CALL_FILL_BENCH_SUBDIR = "bin/call_fill"


def call_fill_atoms(point: tuple[int, ...], addrfilter: str | None) -> int | None:
    """The filler has no branch, so the count is bench_call's at CALL_LEN=1."""
    call_k, iters = point
    return call_atoms((call_k, 1, iters), addrfilter)


CALL_FILL_BENCH = Bench(
    name="call_fill",
    axes=(
        Axis("call_k", "k", CALL_K_LIST, make_var="CALL_K_LIST"),
        Axis(
            "iters", "iters", CALL_FILL_ITERS_LIST, width=10,
            make_var="CALL_FILL_ITERS_LIST",
        ),
    ),
    binary_name=lambda p: f"bench_call_fill_k{p[0]}_i{p[1]}",
    group_depth=0,
    expected_atoms=call_fill_atoms,
)

# Build constants that are not sweep axes, emitted into axes.mk alongside them.
CONSTANT_MAKE_VARS = {"CALL_P": [CALL_P], "CALL_FILL": [CALL_FILL]}

# Every Bench, for the axes emitter.
ALL_BENCHES = (addr_bench("loop"), addr_bench("chain"), CALL_BENCH, CALL_FILL_BENCH)


# ---------------------------------------------------------------------------
# Makefile emission
# ---------------------------------------------------------------------------


def make_vars() -> dict[str, list[int]]:
    """Every axis keyed by its Makefile variable name; shared variables must agree."""
    out: dict[str, list[int]] = dict(CONSTANT_MAKE_VARS)
    for bench in ALL_BENCHES:
        for axis in bench.axes:
            if not axis.make_var:
                continue
            previous = out.setdefault(axis.make_var, axis.values)
            if previous != axis.values:
                raise AxisConflictError(
                    f"{axis.make_var} declared twice with different values: "
                    f"{previous} vs {axis.values}"
                )
    return out


def emit_make() -> str:
    """axes.mk: the sweep axes as Make variables, included by the Makefile."""
    width = max(len(v) for v in make_vars())
    lines = [
        "# Generated by 'python3 -m tracecliff axes --make', do not edit.",
        "# The axis values live in tracecliff/benches.py, edit them there and",
        "# this file is regenerated by the Makefile on the next build.",
        "",
    ]
    lines += [
        f"{var:<{width}} := {' '.join(str(v) for v in values)}"
        for var, values in make_vars().items()
    ]
    return "\n".join(lines) + "\n"


def add_arguments(parser) -> None:
    parser.add_argument(
        "--make", action="store_true", help="emit the axes as Make variables (axes.mk)"
    )


def run(args) -> None:
    if args.make:
        print(emit_make(), end="")
        return
    for var, values in make_vars().items():
        print(f"{var} = {' '.join(str(v) for v in values)}")
