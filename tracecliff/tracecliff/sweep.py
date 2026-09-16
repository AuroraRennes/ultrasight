"""
sweep.py — sweep engine for benchmarks

A sweep is described by a `Bench` (see benches.py): an ordered list of `Axis`
values plus a rule for turning one point into a binary name. The engine walks
every point once per combination, runs cs-trace with -o pointed at a fresh temp
CSV, and merges that row with the point and the factor values into one output CSV.

Must be run as root (cs-trace needs /dev/mem access).

Factors:
    etr     --useetr
            1 = ETR enabled as a trace sink
            0 = ETR skipped, TPIU as the only trace sink (on PL)
    bb      --branchbroadcast
            1 = ETM branch broadcasting on: every E branch gets a full address
                packet after it in the trace
            0 = branch broadcasting off: branches collapse to atom-only for
                predictable branches and need access to the binary disassembly
                to infer control-flow
    stall   --stall
            off, or the ISTALL FIFO level at which the ETM stalls the core
    addrfilter --addrfilter
            where the tracee text range is enforced: etm (ETM address
            comparators), pl (decoder, the ETM traces all of EL0) or none
    bbfilter --bbfilter
            where branch broadcast applies: all, in (inside the tracee text)
            or out (outside it)
"""

import argparse
import csv
import os
import shlex
import subprocess
import sys
import tempfile
import uuid
from collections.abc import Callable, Iterator
from dataclasses import dataclass
from datetime import datetime
from fnmatch import fnmatch
from itertools import product
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# tracecliff lives inside ultrasight, whose own cs-trace build sits one level up
DEFAULT_CS_TRACE = REPO_ROOT.parent / "cs-trace"

# The subset of cs-trace's stats CSV we keep
CST_KEEP = [
    "binary",
    "child_time_s",
    "atom_elem_sum",
    "overflow_count",
    "raw_trace_bytes",
]

# Only get the binary name rather than full path
CST_FIELDS = ["binary_name"] + CST_KEEP[1:]


def projected(row: dict) -> dict:
    """cs-trace's row narrowed to CST_KEEP, ready for our writer."""
    out = {k: row.get(k, "ERR") for k in CST_KEEP}
    out["binary_name"] = str(out.pop("binary")).rsplit("/", 1)[-1]
    return {k: out[k] for k in CST_FIELDS}


# The cs-trace configuration knobs a sweep can vary. Each one is a CSV column
# (its field name) and a cs-trace flag carrying the value verbatim.
#
# Note: these are requested values, recorded as the sweep asked for them.
FACTOR_FLAGS = {
    "etr": "--useetr",
    "bb": "--branchbroadcast",
    "stall": "--stall",
    "addrfilter": "--addrfilter",
    "bbfilter": "--bbfilter",
}

# What a sweep varies if it says nothing
DEFAULT_FACTORS = {"etr": ["0", "1"], "bb": ["0", "1"]}

# Trailing column after the factor columns, appended to each bench's own axes.
RUN_FIELD = "run"


# ---------------------------------------------------------------------------
# Bench description
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Axis:
    field: str

    # Short print label
    label: str
    values: list[int]
    width: int = 4

    # Makefile variable this axis is emitted as by tracecliff axes --make (see benches.py)
    make_var: str = ""


@dataclass(frozen=True)
class Bench:
    """A benchmark family: what to sweep, and which binary each point names.

    name        prefix of the output CSV (e.g. "addr_loop")
    axes        swept parameters, outermost first; the innermost is ITERS
    binary_name maps one point (one value per axis) to a binary filename
    group_depth how many leading axes form a printed block, a blank line is
                emitted whenever that prefix changes
    detail      optional extra banner line, e.g. addr's --kind
    expected_atoms
                maps one point to the atom elements a lossless trace of it
                decodes, or None when no formula covers the benchmark
    """

    name: str
    axes: tuple[Axis, ...]
    binary_name: Callable[[tuple[int, ...]], str]
    group_depth: int = 1
    detail: str = ""
    expected_atoms: Callable[[tuple[int, ...]], int] | None = None

    def sweep_fields(self, factor_fields: list[str]) -> list[str]:
        return [a.field for a in self.axes] + factor_fields + [RUN_FIELD]

    def csv_fields(self, factor_fields: list[str]) -> list[str]:
        return self.sweep_fields(factor_fields) + CST_FIELDS

    def points(self) -> Iterator[tuple[int, ...]]:
        return product(*(a.values for a in self.axes))

    def describe(self, point: tuple[int, ...], pad: bool = False) -> str:
        """ "n=64  s=4096 iters=1000      " (padded) or "n=64 s=4096 iters=1000"."""
        return " ".join(
            f"{a.label}={v:<{a.width}}" if pad else f"{a.label}={v}"
            for a, v in zip(self.axes, point)
        )


# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------


@dataclass
class Config:
    bench: Bench
    cs_trace: Path
    bench_dir: Path
    cs_flags: list[str]
    runs: int

    # Swept cs-trace knobs: field name -> the values to walk, in order. Keys
    # must be in FACTOR_FLAGS. The cross product of these is walked once per
    # bench point.
    factors: dict[str, list[str]]

    # Glob patterns matched against each point's binary name. Empty = the whole
    # bench grid.
    stimuli: tuple[str, ...] = ()

    def points(self) -> list[tuple]:
        """The bench points this sweep visits, after --stimulus filtering."""
        pts = list(self.bench.points())
        if not self.stimuli:
            return pts
        kept = [
            p
            for p in pts
            if any(fnmatch(self.bench.binary_name(p), pat) for pat in self.stimuli)
        ]
        if not kept:
            die(
                "no bench point matches --stimulus "
                f"{list(self.stimuli)}\n       grid holds: "
                + ", ".join(self.bench.binary_name(p) for p in pts[:6])
                + (", ..." if len(pts) > 6 else "")
            )
        return kept

    @property
    def factor_fields(self) -> list[str]:
        return list(self.factors)

    @property
    def csv_fields(self) -> list[str]:
        return self.bench.csv_fields(self.factor_fields)

    def combos(self) -> Iterator[dict[str, str]]:
        """Every factor combination, in declaration order."""
        fields = self.factor_fields
        for values in product(*(self.factors[f] for f in fields)):
            yield dict(zip(fields, values))


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def die(msg: str) -> None:
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def cs_flags_for(cfg: Config, combo: dict[str, str]) -> list[str]:
    flags = list(cfg.cs_flags)
    for field, value in combo.items():
        flags += [FACTOR_FLAGS[field], str(value)]
    return flags


def describe_combo(combo: dict[str, str]) -> str:
    """ "etr=0 bb=1 stall=off", the sweep's own label for a factor cell."""
    return " ".join(f"{k}={v}" for k, v in combo.items()) or "(defaults)"


def run_once(cfg: Config, binary: Path, flags: list[str]) -> dict:
    """Run cs-trace once with -o pointed at a fresh temp CSV, return the
    single resulting row as a dict (all "ERR" on failure/timeout)."""
    # cs-trace creates the file itself, so we only need an unused name.
    tmp_path = Path(tempfile.gettempdir()) / f"cstrace_{uuid.uuid4().hex}.csv"
    cmd = [str(cfg.cs_trace)] + flags + ["-o", str(tmp_path), "--", str(binary)]
    try:
        result = subprocess.run(
            cmd, check=False, capture_output=True, text=True, timeout=300
        )
        if result.returncode != 0:
            print(f"  cs-trace exited {result.returncode}")
    except subprocess.TimeoutExpired:
        print("  cs-trace timed out")

    row = None
    if tmp_path.is_file():
        try:
            with tmp_path.open(newline="") as f:
                rows = list(csv.DictReader(f))
            row = rows[0] if rows else None
        finally:
            tmp_path.unlink(missing_ok=True)

    return row if row is not None else {k: "ERR" for k in CST_KEEP}


def run_point(
    cfg: Config,
    writer: csv.DictWriter,
    point: tuple[int, ...],
    combo: dict[str, str],
) -> None:
    bench = cfg.bench
    binary = cfg.bench_dir / bench.binary_name(point)
    if not binary.is_file():
        print(f"SKIP  {bench.describe(point)} {describe_combo(combo)}  (not found)")
        return

    flags = cs_flags_for(cfg, combo)
    axis_cols = {a.field: v for a, v in zip(bench.axes, point)}
    overflows, times = [], []

    for run in range(1, cfg.runs + 1):
        row = run_once(cfg, binary, flags)
        overflows.append(row.get("overflow_count", "ERR"))
        times.append(row.get("child_time_s", "ERR"))
        writer.writerow({**axis_cols, **combo, RUN_FIELD: run, **projected(row)})

    numeric_times = [float(t) for t in times if t != "ERR"]
    avg_time = (
        f"{sum(numeric_times) / len(numeric_times):.6f}" if numeric_times else "ERR"
    )
    ov_str = "  ".join(str(o).rjust(6) for o in overflows)

    print(
        f"  {describe_combo(combo)}  {bench.describe(point, pad=True)}  "
        f"overflows=[{ov_str}]  avg_time={avg_time}s"
    )


# ---------------------------------------------------------------------------
# Sweep
# ---------------------------------------------------------------------------


def do_sweep(cfg: Config, writer: csv.DictWriter) -> None:
    """The selected bench points, once per factor combination."""
    bench = cfg.bench
    depth = bench.group_depth
    points = cfg.points()

    for combo in cfg.combos():
        print(f"\n## {describe_combo(combo)}")
        group = None
        for point in points:
            if group is not None and point[:depth] != group:
                print()
            group = point[:depth]
            run_point(cfg, writer, point, combo)
        print()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def run(cfg: Config) -> None:
    bench = cfg.bench
    if not Path(cfg.cs_trace).is_file():
        die(
            f"cs-trace binary not found: {cfg.cs_trace}\n"
            "       build it in the ultrasight superproject (make -C ..), "
            "or pass --cs-trace for an out-of-tree build"
        )
    if os.geteuid() != 0:
        die(
            "must be run as root (cs-trace needs /dev/mem) — try: sudo python3 -m tracecliff ..."
        )

    results_dir = REPO_ROOT / "results"
    results_dir.mkdir(exist_ok=True)

    timestamp = datetime.now().astimezone().strftime("%Y%m%d_%H%M%S%z")
    # One tag per swept factor, so the filename still says what varied
    factor_tag = (
        "_".join(
            f"{field}{'-'.join(str(v) for v in values)}"
            for field, values in cfg.factors.items()
        )
        or "defaults"
    )
    csv_path = results_dir / f"{bench.name}_{factor_tag}_{timestamp}.csv"

    print(f"cs-trace : {cfg.cs_trace}")
    print(f"bench_dir: {cfg.bench_dir}")
    print(f"cs_flags : {cfg.cs_flags or '(none)'}")
    print(f"runs/pt  : {cfg.runs}")
    selected = cfg.points()
    print(
        f"stimuli  : {len(selected)} point(s)"
        + (f" matching {list(cfg.stimuli)}" if cfg.stimuli else " (whole grid)")
    )
    if bench.detail:
        print(f"kind     : {bench.detail}")
    for field, values in cfg.factors.items():
        print(f"{field:<9}: {values}  ({FACTOR_FLAGS[field]})")
    print(f"csv      : {csv_path}")

    # buffering=1 (line-buffered): each row hits the file as it is written, so
    # an interrupted sweep still leaves everything it got through on disk.
    with csv_path.open("w", newline="", buffering=1) as f:
        writer = csv.DictWriter(f, fieldnames=cfg.csv_fields)
        writer.writeheader()
        do_sweep(cfg, writer)

    print(f"\nDone. Results: {csv_path}")


# ---------------------------------------------------------------------------
# Argument surface shared by every sweep
# ---------------------------------------------------------------------------


def add_common_arguments(parser: argparse.ArgumentParser, bench_dir_help: str) -> None:
    parser.add_argument(
        "--cs-trace",
        default=DEFAULT_CS_TRACE,
        type=Path,
        help=f"path to cs-trace binary (default: {DEFAULT_CS_TRACE}, "
        "the ultrasight superproject's own build)",
    )
    parser.add_argument("--bench-dir", type=Path, default=None, help=bench_dir_help)
    parser.add_argument(
        "--cs-flags",
        default="",
        help='fixed flags passed to cs-trace for every run, e.g. "-b ZCU-104 -c 0"',
    )
    parser.add_argument(
        "--runs", type=int, default=3, help="repetitions per point (default: 3)"
    )
    parser.add_argument(
        "--stimulus",
        action="append",
        default=[],
        metavar="GLOB",
        help="only run bench points whose binary name matches GLOB, "
        "e.g. --stimulus 'bench_n2_s0_i*'. Repeatable (a point "
        "matching any pattern is kept). Default: the whole grid",
    )
    add_factor_arguments(parser)


def add_factor_arguments(parser: argparse.ArgumentParser) -> None:
    """--etr-list/--bb-list plus the general --factor NAME=V1,V2 form."""
    parser.add_argument(
        "--etr-list",
        nargs="+",
        default=None,
        help="--useetr values to sweep (default: 0 1)",
    )
    parser.add_argument(
        "--bb-list",
        nargs="+",
        default=None,
        help="--branchbroadcast values to sweep (default: 0 1)",
    )
    parser.add_argument(
        "--factor",
        action="append",
        default=[],
        metavar="NAME=V1,V2",
        help="sweep cs-trace knob NAME over the listed values, e.g. "
        "--factor stall=off,3 --factor addrfilter=etm,none. "
        f"NAME is one of: {', '.join(FACTOR_FLAGS)}. Repeatable; "
        "the cross product of every --factor is walked. Giving any "
        "--factor replaces the default etr/bb sweep unless they are "
        "also named (by --factor or --etr-list/--bb-list)",
    )


def factors_from_args(args: argparse.Namespace) -> dict[str, list[str]]:
    """The swept factors, in the order they were named on the command line."""
    factors: dict[str, list[str]] = {}
    for spec in args.factor:
        field, sep, values = spec.partition("=")
        if not sep:
            die(f"--factor needs NAME=V1,V2 (got {spec!r})")
        if field not in FACTOR_FLAGS:
            die(
                f"unknown factor {field!r} (expected one of: {', '.join(FACTOR_FLAGS)})"
            )
        factors[field] = [v for v in values.split(",") if v]
        if not factors[field]:
            die(f"--factor {field} has no values")

    # The named options win over --factor for the same field.
    for field, value in (("etr", args.etr_list), ("bb", args.bb_list)):
        if value is not None:
            factors[field] = [str(v) for v in value]

    return factors or {k: list(v) for k, v in DEFAULT_FACTORS.items()}


def config_from_args(
    args: argparse.Namespace, bench: Bench, default_subdir: str
) -> Config:
    return Config(
        bench=bench,
        cs_trace=args.cs_trace,
        bench_dir=Path(args.bench_dir or (REPO_ROOT / default_subdir)),
        cs_flags=shlex.split(args.cs_flags),
        runs=args.runs,
        factors=factors_from_args(args),
        stimuli=tuple(args.stimulus),
    )
