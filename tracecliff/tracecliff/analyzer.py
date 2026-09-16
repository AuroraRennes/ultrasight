"""
analyzer.py — compute CoreSight ETM overflow-sweep results.

    tracecliff analyze [files...] [--csv-out]

reporter.py prints the text report from the points computed here.
"""

import argparse
import csv
import sys
from collections import defaultdict
from dataclasses import dataclass, fields
from pathlib import Path

from tracecliff.benches import ATOMS_PER_BRANCH
from tracecliff.exception import SweepCsvError
from tracecliff.points import bench_for, load_points
from tracecliff.sweep import FACTOR_FLAGS

REPO_ROOT = Path(__file__).resolve().parent.parent
RESULTS_DIR = REPO_ROOT / "results"
ANALYZED_DIR_DEFAULT = RESULTS_DIR / "analyzed_results"

# Ground truth. edges_total is NOT usable for addr: where the ETM's address
# cache half-hits it undercounts by 3-10% while atoms stay exact.
TRUE_METRIC = "atom_elem_sum"

# The dependent variable of every configuration sweep.
OUTCOME_FIELD = "overflow_count"

# Shortest run whose bytes/atom is trustworthy: below it the fixed trace-enable
# preamble is a visible fraction of raw_trace_bytes.
COST_MIN_ITERS = 100_000

# A run is lossless when it lands within this much of the analytic count.
LOSSLESS_TOL = 0.02
LOSSLESS_ABS_SLACK = 256


# ---------------------------------------------------------------------------
# Ground truth
# ---------------------------------------------------------------------------


def slack(count):
    """Tolerance around count"""
    return max(count * LOSSLESS_TOL, LOSSLESS_ABS_SLACK) if count else 0


def is_lossless(count, base):
    return count >= base - slack(base)


def true_count(point):
    return point.mean(TRUE_METRIC)


def by_workload(points):
    """Points that measured anything, grouped under their workload."""
    out = defaultdict(list)
    for key, p in points.items():
        if true_count(p) is not None:
            out[key.workload].append(p)
    return out


def best_run(ps):
    """The point a workload's baseline leans on, and whether it overflowed."""
    best = max(ps, key=true_count)
    return best, best.mean("overflow_count") or 0


# ---------------------------------------------------------------------------
# Baseline / loss computation
# ---------------------------------------------------------------------------


# Provenance of a baseline read from the runs because no formula covers the
# workload: the one case where an overflowed best run understates the reference.
REF_NO_ANALYTIC = "measured-max (no analytic count for this config)"


def workload_baselines(points, bench):
    """Per-workload lossless reference count and its provenance.
    Analytic where derivable; measured runs only corroborate it."""
    baselines = {}
    for wkey, ps in by_workload(points).items():
        expected = bench.expected_atoms(*wkey) if bench.expected_atoms else None
        best, best_ovf = best_run(ps)
        observed = true_count(best)

        if expected is None:
            baselines[wkey] = observed, REF_NO_ANALYTIC
        elif observed > expected + slack(expected):
            # The formula understates the branches, or the decoder inflated
            # (framing desync) — either way it is not usable as a ceiling.
            baselines[wkey] = (
                observed,
                "measured-max (ANALYTIC COUNT BELOW OBSERVED - formula suspect)",
            )
        elif observed >= expected - slack(expected):
            # A run that overflowed lost data by definition, so it corroborates
            # nothing — and must not silently become a ceiling.
            baselines[wkey] = (
                expected,
                "analytic (corroborated by a lossless run)"
                if best_ovf == 0
                else "analytic (nearest run OVERFLOWED - not corroborated)",
            )
        else:
            # Every arm lost data. A real finding, not a fallback.
            baselines[wkey] = expected, "analytic (NO run reached it - all arms lossy)"
    return baselines


def reference_bytes_per_atom(points, baselines):
    """Trace bytes per atom, per (axes but iters, bb), from its longest clean runs.
    Short runs inflate it (preamble), lossy runs understate it."""
    per_key = defaultdict(list)
    for key, p in points.items():
        base = baselines.get(key.workload)
        raw, true = p.mean("raw_trace_bytes"), true_count(p)
        if p.mean("overflow_count") or not base or not raw or not true:
            continue
        if key.iters < COST_MIN_ITERS or not is_lossless(true, base):
            continue
        per_key[(key.axes[:-1], key.arm("bb"))].append((key.iters, raw / true))

    out = {}
    for k, vals in per_key.items():
        longest = max(i for i, _ in vals)
        best = [bpa for i, bpa in vals if i == longest]
        out[k] = sum(best) / len(best)
    return out


def reference_branch_times(points, baselines):
    """Per-workload seconds-per-branch from its least-stalled lossless run,
    free of the stall bias a lossy run's own average carries."""
    best = {}
    for key, p in points.items():
        wkey = key.workload
        base = baselines.get(wkey)
        secs, count = p.mean("child_time_s"), true_count(p)
        if not base or not secs or not count or not is_lossless(count, base):
            continue
        per_branch = secs / base
        if wkey not in best or per_branch < best[wkey]:
            best[wkey] = per_branch
    return best


@dataclass(frozen=True)
class Loss:
    """What a point lost against its workload's baseline, and the rates around it.

    Field order is the analyzed CSV's column order.
    """

    baseline_edges: float | None = None
    # Provenance, on every measured point of the workload.
    baseline_ref: str | None = None
    loss_pct: float | None = None
    # Over-count means the decoder desynced, not "extra data"; without this
    # flag the clamp on loss_pct would read it as 0% loss.
    inflated: bool = False
    # offered = what the workload generated
    offered_edge_rate: float | None = None
    # delivered = what came back
    delivered_edge_rate: float | None = None
    # bytes/s = what the sink moved
    bytes_per_s: float | None = None
    # An estimate on lossy points: the baseline atom count priced at the
    # packet cost of this workload's own clean runs.
    offered_byte_rate: float | None = None
    # Escape window (ARMOR's Delta-t): branches that never arrived, spread over
    # the overflows, at the workload's unstalled branch rate.
    missing_branches: float | None = None
    escape_window_s: float | None = None
    escape_window_ref: str | None = None


def point_loss(key, p, baseline, branch_time, bytes_per_atom):
    """The Loss of one point, given its workload's (count, provenance) baseline."""
    base, ref = baseline
    count, secs = true_count(p), p.mean("child_time_s")
    if count is None:
        return Loss(baseline_edges=base)
    if not base:
        return Loss(baseline_edges=base, baseline_ref=ref)

    offered = delivered = bytes_per_s = offered_bytes = None
    if secs:
        offered, delivered = base / secs, count / secs
        if p.mean("raw_trace_bytes"):
            bytes_per_s = p.mean("raw_trace_bytes") / secs
        if bytes_per_atom:
            offered_bytes = base * bytes_per_atom / secs

    inflated = count > base + slack(base)
    missing = max(0.0, base - count)
    window_ref = None
    if branch_time is not None:
        window_ref = "lossless-arm"
    elif secs:
        branch_time = secs / base  # fall back to this run's own rate
        window_ref = "self (stall-biased)"
    n_ovf = p.mean("overflow_count")
    # Meaningless where true_count is a framing artefact rather than a count.
    window = None
    if n_ovf and missing and branch_time and not inflated:
        window = (missing / n_ovf) * branch_time

    return Loss(
        baseline_edges=base,
        baseline_ref=ref,
        loss_pct=max(0.0, (base - count) / base * 100.0),
        inflated=inflated,
        offered_edge_rate=offered,
        delivered_edge_rate=delivered,
        bytes_per_s=bytes_per_s,
        offered_byte_rate=offered_bytes,
        missing_branches=missing,
        escape_window_s=window,
        escape_window_ref=window_ref,
    )


def attach_losses(points, bench):
    """Set every point's Loss, each built in one go."""
    baselines = workload_baselines(points, bench)
    counts = {wkey: base for wkey, (base, _) in baselines.items()}
    branch_times = reference_branch_times(points, counts)
    bytes_per_atom = reference_bytes_per_atom(points, counts)
    for key, p in points.items():
        p.loss = point_loss(
            key,
            p,
            baselines.get(key.workload, (None, None)),
            branch_times.get(key.workload),
            bytes_per_atom.get((key.axes[:-1], key.arm("bb"))),
        )


# Analyzed CSV columns after the key: each read off the point, then its Loss.
CSV_POINT_FIELDS = {
    "n_runs": lambda p: p.n_runs,
    "n_ok": lambda p: p.n_ok(TRUE_METRIC),
    "atom_elem_sum": lambda p: p.mean("atom_elem_sum"),
    "true_count": true_count,
    "overflow_count": lambda p: p.mean("overflow_count"),
    "child_time_s": lambda p: p.mean("child_time_s"),
}


def write_csv_out(points, bench, out_path):
    keys = sorted(points)
    arm_fields = [name for name, _ in keys[0].arms]
    with out_path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(
            [a.field for a in bench.axes]
            + arm_fields
            + list(CSV_POINT_FIELDS)
            + [f.name for f in fields(Loss)]
        )
        for key in keys:
            p = points[key]
            w.writerow(
                list(key.axes)
                + [v for _, v in key.arms]
                + [get(p) for get in CSV_POINT_FIELDS.values()]
                + [getattr(p.loss, f.name) for f in fields(Loss)]
            )
    print(f"  wrote {out_path}")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def analyze_file(path, csv_out):
    bench = bench_for(path)
    if bench is None:
        print(f"\n{path}: no bench name prefix in the filename, skipping")
        return None
    try:
        points = load_points(path, bench)
    except SweepCsvError as e:
        print(f"\n{e}, skipping")
        return None
    if not points:
        print(f"\n{path}: empty, skipping")
        return None

    attach_losses(points, bench)

    # Imported here: reporter imports back from this module.
    from tracecliff import reporter

    reporter.report(points, bench, path.name)

    if csv_out:
        ANALYZED_DIR_DEFAULT.mkdir(parents=True, exist_ok=True)
        write_csv_out(points, bench, ANALYZED_DIR_DEFAULT / f"analyzed_{path.stem}.csv")

    return bench, points


# ---------------------------------------------------------------------------
# Reference arm
# ---------------------------------------------------------------------------

# Every factor at its least-invasive level. A point matching this on the factors
# its CSV actually swept is what the other arms are differenced against.
REFERENCE_ARMS = {
    "stall": "off",
    "etr": "0",
    "bb": "0",
    "addrfilter": "etm",
    "bbfilter": "all",
}
# addr_loop n2 s0: 914.8 MB/s, the highest offered rate still returning
# overflow_count=0, so every factor reads as an increase from zero.
REFERENCE_BINARY = "bench_n2_s0_i10000000"


def is_reference_arms(key, reference):
    """Is this point at the reference level for every factor the CSV swept?"""
    arms = dict(key.arms)
    return all(arms.get(k) == v for k, v in reference.items() if k in arms)


def find_reference(points, reference):
    """The reference point per workload, plus the global one if present."""
    per_workload = {}
    for key, point in points.items():
        if is_reference_arms(key, reference):
            per_workload.setdefault(key.axes, point)
    global_ref = next(
        (p for p in per_workload.values() if p.binary_name == REFERENCE_BINARY), None
    )
    return per_workload, global_ref


def branches(point):
    """Executed branches, from the baseline count."""
    base = point.loss.baseline_edges if point.loss else None
    if base:
        return base / ATOMS_PER_BRANCH
    return point.key.iters


def delivered(point):
    """Fraction of the expected atom stream that arrived, or None."""
    pct = point.loss.loss_pct if point.loss else None
    return None if pct is None else 1.0 - pct / 100.0


def add_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "files",
        nargs="*",
        type=Path,
        help="CSV files to analyze (default: results/*.csv)",
    )
    parser.add_argument(
        "--csv-out",
        action="store_true",
        help="also write results/analyzed_<name>.csv per-point summaries",
    )


def _input_files(args):
    if args.files:
        return args.files
    if not RESULTS_DIR.is_dir():
        print(
            f"ERROR: no files given and {RESULTS_DIR} does not exist", file=sys.stderr
        )
        sys.exit(1)
    return [
        f
        for f in sorted(RESULTS_DIR.glob("*.csv"))
        if not f.name.startswith("analyzed_")
    ]


def run(args: argparse.Namespace) -> None:
    files = _input_files(args)
    if not files:
        print(f"ERROR: no CSV files found in {RESULTS_DIR}", file=sys.stderr)
        sys.exit(1)

    for path in files:
        if not path.is_file():
            print(f"ERROR: {path} not found", file=sys.stderr)
            continue
        analyze_file(path, args.csv_out)

    print(f"\n{'=' * 78}\nAnalyzed {len(files)} file(s).")


def add_arm_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "files",
        nargs="*",
        type=Path,
        help="CSV files to report (default: results/*.csv)",
    )
    parser.add_argument(
        "--per-branch",
        action="store_true",
        help="normalise every metric by branches executed, so runs of "
        "different lengths are comparable",
    )
    parser.add_argument(
        "--reference",
        action="append",
        default=[],
        metavar="NAME=VALUE",
        help="override one factor's reference level, e.g. "
        "--reference addrfilter=none. Repeatable; defaults to "
        + ", ".join(f"{k}={v}" for k, v in REFERENCE_ARMS.items()),
    )


def arms_file(path, per_branch, reference):
    """One CSV reported per arm, or None where it holds no reportable points."""
    bench = bench_for(path)
    if bench is None:
        print(f"\n{path}: no bench name prefix in the filename, skipping")
        return None
    try:
        points = load_points(path, bench)
    except SweepCsvError as e:
        print(f"\n{e}, skipping")
        return None
    if not points:
        print(f"\n{path}: empty, skipping")
        return None
    if not any(p.n_ok(OUTCOME_FIELD) for p in points.values()):
        # Archived pre-qualification CSVs record etr_overflow_count instead, a
        # column points.py no longer maps, so the outcome is absent entirely.
        print(f"\n{'=' * 78}\n{path.name}")
        print(
            f"SKIPPED: no '{OUTCOME_FIELD}' column. This looks like a "
            "pre-qualification CSV, whose dependent variable this report "
            f"cannot show.\n         Read it with: tracecliff analyze {path.as_posix()}"
        )
        return None

    attach_losses(points, bench)

    # Imported here: reporter imports back from this module.
    from tracecliff import reporter

    reporter.report_arms(points, bench, path.name, per_branch, reference)
    return bench, points


def run_arms(args: argparse.Namespace) -> None:
    reference = dict(REFERENCE_ARMS)
    for spec in args.reference:
        field, sep, value = spec.partition("=")
        if not sep or field not in FACTOR_FLAGS:
            print(
                f"ERROR: --reference needs NAME=VALUE with NAME one of: "
                f"{', '.join(FACTOR_FLAGS)} (got {spec!r})",
                file=sys.stderr,
            )
            sys.exit(1)
        reference[field] = value

    files = _input_files(args)
    if not files:
        print(f"ERROR: no CSV files found in {RESULTS_DIR}", file=sys.stderr)
        sys.exit(1)

    for path in files:
        if not path.is_file():
            print(f"ERROR: {path} not found", file=sys.stderr)
            continue
        arms_file(path, args.per_branch, reference)

    print(f"\n{'=' * 78}\nReported {len(files)} file(s).")


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    add_arguments(parser)
    run(parser.parse_args(argv))
