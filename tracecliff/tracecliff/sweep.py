"""
sweep.py — CoreSight ETM overflow characterization sweep

Invoked as a tracecliff subcommand (see tracecliff/cli.py):
    sudo python3 -m tracecliff sweep-addr \\
        --cs-trace /path/to/ultrasight/cs-trace \\
        --cs-flags "-b ZCU-104 -c 0"

Must be run as root (cs-trace needs /dev/mem access).

Each cs-trace invocation is run with -o/--csv pointed at a fresh temp file.
That row is merged with this sweep's own parameters (n_targets, spacing,
iters, etr, run) into the combined output CSV.

Flags:
    --cs-trace    path to cs-trace binary (required)
    --bench-dir   directory containing bench_n*_s*_i* binaries (default: bin/addr_loop)
    --cs-flags    fixed flags passed to cs-trace for every run (e.g. "-b ZCU-104 -c 0")
    --runs        repetitions per point (default: 3)
    --etr-list    --useetr values to sweep (default: 0 1)
                  1 = ETR enabled as a trace sink (default)
                  0 = ETR skipped, TPIU as the only trace sink (on PL)
    --bb-list     --branchbroadcast values to sweep (default: 0 1)
                  1 = ETM branch broadcasting on: every E branch gets a full address
                      packet after it in the trace
                  0 = branch broadcasting off: branches collapse to atom-only for
                      predictable branches and need access to the binary disassembly
                      to infer control-flow
"""

import argparse
import csv
import os
import shlex
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

DEFAULT_BENCH_SUBDIR = "bin/addr_loop"
BIN_PREFIX = "bench"

# Sweep axes — must match Makefile's ADDR_N_LIST / ADDR_STRIDE_LIST / ITERS_LIST
N_LIST = [1, 2, 3, 4, 8, 16, 32, 64]
S_LIST = [0, 64, 128, 256, 512, 1024, 2048, 4096]
ITERS_LIST = [100, 1_000, 10_000, 100_000, 1_000_000, 10_000_000, 100_000_000]

SWEEP_FIELDS = ["n_targets", "spacing", "iters", "etr", "bb", "run"]

# cs-trace's own CSV row schema
CST_FIELDS = [
    "binary",
    "child_time_s",
    "instr_time_s",
    "global_time_s",
    "edges_total",
    "edges_fifo_overflow",
    "edges_freeze_drop",
    "total_cycles",
    "idle_cycles",
    "pkt_cnt",
    "pkt_bst_cnt",
    "pkt_min_bst",
    "pkt_max_bst",
    "pkt_sum_bst",
    "pkt_gap_cnt",
    "pkt_min_gap",
    "pkt_max_gap",
    "pkt_sum_gap",
    "pkt_gap_h_0",
    "pkt_gap_h_1_4",
    "pkt_gap_h_5_15",
    "pkt_gap_h_16_255",
    "pkt_gap_h_256p",
    "pkt_bst_h_1",
    "pkt_bst_h_2_3",
    "pkt_bst_h_4_15",
    "pkt_bst_h_16p",
    "atom_pkt_cnt",
    "atom_pkt_bst_cnt",
    "atom_pkt_min_bst",
    "atom_pkt_max_bst",
    "atom_pkt_sum_bst",
    "atom_pkt_gap_cnt",
    "atom_pkt_min_gap",
    "atom_pkt_max_gap",
    "atom_pkt_sum_gap",
    "atom_pkt_gap_h_0",
    "atom_pkt_gap_h_1_4",
    "atom_pkt_gap_h_5_15",
    "atom_pkt_gap_h_16_255",
    "atom_pkt_gap_h_256p",
    "atom_pkt_bst_h_1",
    "atom_pkt_bst_h_2_3",
    "atom_pkt_bst_h_4_15",
    "atom_pkt_bst_h_16p",
    "atom_elem_sum",
    "atom_elem_h_1",
    "atom_elem_h_2",
    "atom_elem_h_3",
    "atom_elem_h_4",
    "atom_elem_h_5",
    "atom_elem_h_6_11",
    "atom_elem_h_12_24",
    "frame_errors",
    "bs_gen_errors",
    "demux_errors",
    "overflow_count",
    "raw_trace_bytes",
]

CSV_FIELDS = SWEEP_FIELDS + CST_FIELDS


# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------


@dataclass
class Config:
    cs_trace: str
    bench_dir: Path
    cs_flags: list[str]
    runs: int
    etr_list: list[int]
    bb_list: list[int]


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def die(msg: str) -> None:
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def cs_flags_for(cfg: Config, etr: int, bb: int) -> list[str]:
    return cfg.cs_flags + ["--useetr", str(etr), "--branchbroadcast", str(bb)]


def binary_path(cfg: Config, n: int, s: int, iters: int) -> Path:
    return cfg.bench_dir / f"{BIN_PREFIX}_n{n}_s{s}_i{iters}"


def run_once(cfg: Config, binary: Path, flags: list[str]) -> dict:
    """Run cs-trace once with -o pointed at a fresh temp CSV, return the
    single resulting row as a dict (all "ERR" on failure/timeout)."""
    tmp_path = Path(tempfile.mktemp(suffix=".csv", prefix="cstrace_"))
    cmd = [cfg.cs_trace] + flags + ["-o", str(tmp_path), "--", str(binary)]
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

    return row if row is not None else {k: "ERR" for k in CST_FIELDS}


def run_point(
    cfg: Config,
    writer: csv.DictWriter,
    n: int,
    s: int,
    iters: int,
    etr: int,
    bb: int,
) -> None:
    binary = binary_path(cfg, n, s, iters)
    if not binary.is_file():
        print(f"SKIP  n={n} s={s} iters={iters} etr={etr} bb={bb}  (not found)")
        return

    flags = cs_flags_for(cfg, etr, bb)
    overflows, times = [], []

    for run in range(1, cfg.runs + 1):
        row = run_once(cfg, binary, flags)
        overflows.append(row.get("overflow_count", "ERR"))
        times.append(row.get("child_time_s", "ERR"))
        writer.writerow(
            {
                "n_targets": n,
                "spacing": s,
                "iters": iters,
                "etr": etr,
                "bb": bb,
                "run": run,
                **row,
            }
        )

    numeric_times = [float(t) for t in times if t != "ERR"]
    avg_time = (
        f"{sum(numeric_times) / len(numeric_times):.6f}" if numeric_times else "ERR"
    )
    ov_str = "  ".join(str(o).rjust(6) for o in overflows)

    print(
        f"  etr={etr} bb={bb}  n={n:<4} s={s:<4} iters={iters:<10}  "
        f"overflows=[{ov_str}]  avg_time={avg_time}s"
    )


# ---------------------------------------------------------------------------
# Sweep
# ---------------------------------------------------------------------------


def do_sweep(cfg: Config, writer: csv.DictWriter) -> None:
    for etr in cfg.etr_list:
        for bb in cfg.bb_list:
            print(f"\n## etr={etr} bb={bb}")
            for n in N_LIST:
                for s in S_LIST:
                    for iters in ITERS_LIST:
                        run_point(cfg, writer, n, s, iters, etr, bb)
                print()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def run(cfg: Config) -> None:
    if not Path(cfg.cs_trace).is_file():
        die(f"cs-trace binary not found: {cfg.cs_trace}")
    if os.geteuid() != 0:
        die(
            "must be run as root (cs-trace needs /dev/mem) — try: sudo python3 -m tracecliff sweep-addr ..."
        )

    results_dir = REPO_ROOT / "results"
    results_dir.mkdir(exist_ok=True)

    timestamp = datetime.now().astimezone().strftime("%Y%m%d_%H%M%S%z")
    etr_tag = "-".join(str(e) for e in sorted(set(cfg.etr_list)))
    bb_tag = "-".join(str(b) for b in sorted(set(cfg.bb_list)))
    csv_path = results_dir / f"addr_loop_etr{etr_tag}_bb{bb_tag}_{timestamp}.csv"

    print(f"cs-trace : {cfg.cs_trace}")
    print(f"bench_dir: {cfg.bench_dir}")
    print(f"cs_flags : {cfg.cs_flags or '(none)'}")
    print(f"runs/pt  : {cfg.runs}")
    print(f"etr_list : {cfg.etr_list}")
    print(f"bb_list  : {cfg.bb_list}")
    print(f"csv      : {csv_path}")

    with csv_path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        writer.writeheader()
        do_sweep(cfg, writer)

    print(f"\nDone. Results: {csv_path}")


def add_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--cs-trace", required=True, help="path to cs-trace binary")
    parser.add_argument(
        "--bench-dir",
        type=Path,
        default=None,
        help="directory containing bench_n*_s*_i* binaries (default: bin/addr_loop)",
    )
    parser.add_argument(
        "--cs-flags",
        default="",
        help='fixed flags passed to cs-trace for every run, e.g. "-b ZCU-104 -c 0"',
    )
    parser.add_argument(
        "--runs", type=int, default=3, help="repetitions per point (default: 3)"
    )
    parser.add_argument(
        "--etr-list",
        nargs="+",
        type=int,
        default=[0, 1],
        help="--useetr values to sweep (default: 0 1)",
    )
    parser.add_argument(
        "--bb-list",
        nargs="+",
        type=int,
        default=[0, 1],
        help="--branchbroadcast values to sweep (default: 0 1)",
    )


def config_from_args(args: argparse.Namespace) -> Config:
    bench_dir = args.bench_dir or (REPO_ROOT / DEFAULT_BENCH_SUBDIR)
    return Config(
        cs_trace=args.cs_trace,
        bench_dir=Path(bench_dir),
        cs_flags=shlex.split(args.cs_flags),
        runs=args.runs,
        etr_list=args.etr_list,
        bb_list=args.bb_list,
    )


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    add_arguments(parser)
    args = parser.parse_args(argv)
    run(config_from_args(args))
