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
ADDR_N_LIST = [1, 2, 3, 4, 8, 16, 32, 64]
# STUB_STRIDE: byte spacing between stub targets, 0 (compiler packing) to a page.
ADDR_STRIDE_LIST = [0, 64, 128, 256, 512, 1024, 2048, 4096]
# ITERS: run length, swept to find the shortest run that is in steady state,
# with and without ETR.
ITERS_LIST = [100_000, 1_000_000, 10_000_000, 100_000_000]

# Two build modes of the same source, differing only in binary name.
ADDR_BENCH_SUBDIR = {"loop": "bin/addr_loop", "chain": "bin/addr_chain"}
ADDR_BIN_PREFIX = {"loop": "bench", "chain": "bench_chain"}

# bench_addr.c: every executed branch costs exactly 2 atom elements, so the
# analytic count is 2 x branches.
ATOMS_PER_BRANCH = 2
ADDR_LOOP_UNROLL = 8  # iterations per unrolled block
ADDR_LOOP_INDIRECT_PER_ITER = 2  # the indirect call and its return
# A lap of N hops is the BL to t0, N-1 BRs and the final RET (N+1 branches),
# plus the loop branch in bench() that starts the next lap.
ADDR_CHAIN_LAP_OVERHEAD_BRANCHES = 2


def addr_loop_atoms(point: tuple[int, ...]) -> int:
    _n_targets, _spacing, iters = point
    # One loop-back branch per block of ADDR_LOOP_UNROLL calls, then one per
    # call in the tail loop that runs the remainder.
    blocks, tail = divmod(iters, ADDR_LOOP_UNROLL)
    branches = iters * ADDR_LOOP_INDIRECT_PER_ITER + blocks + tail
    return branches * ATOMS_PER_BRANCH


def addr_chain_atoms(point: tuple[int, ...]) -> int:
    n_targets, _spacing, iters = point
    # The chain only runs whole laps of N hops: iters rounds up to one.
    laps = -(-iters // n_targets)  # ceil
    branches = laps * (n_targets + ADDR_CHAIN_LAP_OVERHEAD_BRANCHES)
    return branches * ATOMS_PER_BRANCH


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

# CALL_EVERY: iterations between successive PLT calls into libc, the INVERSE of
# crossing density (larger = crossing-sparser). 0 never calls: the control
# point, same loop and no range crossing, where addrfilter=etm and none agree.
CALL_EVERY_LIST = [0, 1, 4, 16, 64]
# ITERS for call binaries only, independent of the addr families' ITERS_LIST.
CALL_ITERS_LIST = [10_000_000]

CALL_BENCH_SUBDIR = "bin/call"

CALL_BENCH = Bench(
    name="call",
    axes=(
        Axis("call_every", "c", CALL_EVERY_LIST, make_var="CALL_LIST"),
        Axis("iters", "iters", CALL_ITERS_LIST, width=10, make_var="CALL_ITERS_LIST"),
    ),
    binary_name=lambda p: f"bench_call_c{p[0]}_i{p[1]}",
    # one printed block per CALL_EVERY, i.e. its ITERS row
    group_depth=1,
    # No formula: strlen's libc branches are traced or not depending on
    # addrfilter, so the atom count is not a property of the binary alone.
    expected_atoms=None,
)


# Every Bench, for the axes emitter.
ALL_BENCHES = (addr_bench("loop"), addr_bench("chain"), CALL_BENCH)


# ---------------------------------------------------------------------------
# Makefile emission
# ---------------------------------------------------------------------------


def make_vars() -> dict[str, list[int]]:
    """Every axis keyed by its Makefile variable name; shared variables must agree."""
    out: dict[str, list[int]] = {}
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
