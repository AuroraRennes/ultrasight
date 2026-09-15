"""
analyzer.py — compute CoreSight ETM overflow-sweep results.

    tracecliff analyze [files...] [--csv-out]

reporter.py prints the text report from the points computed here.
"""

import argparse
import csv
import sys
from collections import defaultdict
from pathlib import Path

from tracecliff.exception import SweepCsvError
from tracecliff.points import bench_for, load_points

REPO_ROOT = Path(__file__).resolve().parent.parent
RESULTS_DIR = REPO_ROOT / "results"
ANALYZED_DIR_DEFAULT = RESULTS_DIR / "analyzed_results"

# Ground truth. edges_total is NOT usable for addr: where the ETM's address
# cache half-hits it undercounts by 3-10% while atoms stay exact.
TRUE_METRIC = "atom_elem_sum"

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


def compute_baselines(points, bench):
    """Per-workload lossless reference counts, tagging provenance on each point.
    Analytic where derivable; measured runs only corroborate it."""
    baselines = {}
    for wkey, ps in by_workload(points).items():
        expected = bench.expected_atoms(wkey) if bench.expected_atoms else None
        best, best_ovf = best_run(ps)
        observed = true_count(best)

        if expected is None:
            baselines[wkey] = observed
            ref = REF_NO_ANALYTIC
        elif observed > expected + slack(expected):
            # The formula understates the branches, or the decoder inflated
            # (framing desync) — either way it is not usable as a ceiling.
            baselines[wkey] = observed
            ref = "measured-max (ANALYTIC COUNT BELOW OBSERVED - formula suspect)"
        elif observed >= expected - slack(expected):
            baselines[wkey] = expected
            # A run that overflowed lost data by definition, so it corroborates
            # nothing — and must not silently become a ceiling.
            ref = (
                "analytic (corroborated by a lossless run)"
                if best_ovf == 0
                else "analytic (nearest run OVERFLOWED - not corroborated)"
            )
        else:
            # Every arm lost data. A real finding, not a fallback.
            baselines[wkey] = expected
            ref = "analytic (NO run reached it - all arms lossy)"

        # Stored on every point of the workload: one home for provenance, which
        # is what carries it into the analyzed CSV.
        for p in ps:
            p.loss = {"baseline_ref": ref}

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


# Every key annotate() derives into Point.loss, so a point that derives nothing
# still carries the full set (the CSV writer reads them all).
DERIVED_DEFAULTS = {
    "loss_pct": None,
    "baseline_edges": None,
    "inflated": False,
    "offered_edge_rate": None,
    "delivered_edge_rate": None,
    "bytes_per_s": None,
    "offered_byte_rate": None,
    "missing_branches": None,
    "escape_window_s": None,
    "escape_window_ref": None,
}


def annotate(points, baselines, ref_branch_times, ref_bytes_per_atom):
    for key, p in points.items():
        wkey = key.workload
        base = baselines.get(wkey)
        agg = p.loss = {"baseline_ref": None, **(p.loss or {}), **DERIVED_DEFAULTS}
        agg["baseline_edges"] = base
        count, secs = true_count(p), p.mean("child_time_s")
        if not base or count is None:
            continue

        agg["loss_pct"] = max(0.0, (base - count) / base * 100.0)
        if secs:
            # offered = what the workload generated, delivered = what came back,
            # bytes_per_s = what the sink moved (the quantity ETR limits).
            agg["offered_edge_rate"] = base / secs
            agg["delivered_edge_rate"] = count / secs
            if p.mean("raw_trace_bytes"):
                agg["bytes_per_s"] = p.mean("raw_trace_bytes") / secs
            # An estimate on lossy points: the baseline atom count priced at
            # the packet cost of this workload's own clean runs.
            bpa = ref_bytes_per_atom.get((key.axes[:-1], key.arm("bb")))
            if bpa:
                agg["offered_byte_rate"] = base * bpa / secs

        # Over-count means the decoder desynced, not "extra data"; without this
        # flag the clamp above would read it as 0% loss.
        agg["inflated"] = count > base + slack(base)

        # Escape window (ARMOR's Delta-t): branches that never arrived, spread
        # over the overflows, at the workload's unstalled branch rate.
        agg["missing_branches"] = missing = max(0.0, base - count)
        ref = ref_branch_times.get(wkey)
        if ref is not None:
            agg["escape_window_ref"] = "lossless-arm"
        elif secs:
            ref = secs / base  # fall back to this run's own rate
            agg["escape_window_ref"] = "self (stall-biased)"
        n_ovf = p.mean("overflow_count")
        # Meaningless where true_count is a framing artefact rather than a count.
        if n_ovf and missing and ref and not agg["inflated"]:
            agg["escape_window_s"] = (missing / n_ovf) * ref


# Analyzed CSV columns after the key: each read off the point, then its loss.
CSV_POINT_FIELDS = {
    "n_runs": lambda p: p.n_runs,
    "n_ok": lambda p: p.n_ok(TRUE_METRIC),
    "atom_elem_sum": lambda p: p.mean("atom_elem_sum"),
    "true_count": true_count,
    "overflow_count": lambda p: p.mean("overflow_count"),
    "child_time_s": lambda p: p.mean("child_time_s"),
}
CSV_LOSS_FIELDS = [
    "baseline_edges",
    "baseline_ref",
    "loss_pct",
    "inflated",
    "offered_edge_rate",
    "delivered_edge_rate",
    "bytes_per_s",
    "offered_byte_rate",
    "missing_branches",
    "escape_window_s",
    "escape_window_ref",
]


def write_csv_out(points, bench, out_path):
    keys = sorted(points)
    arm_fields = [name for name, _ in keys[0].arms]
    with out_path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(
            [a.field for a in bench.axes]
            + arm_fields
            + list(CSV_POINT_FIELDS)
            + CSV_LOSS_FIELDS
        )
        for key in keys:
            p = points[key]
            w.writerow(
                list(key.axes)
                + [v for _, v in key.arms]
                + [get(p) for get in CSV_POINT_FIELDS.values()]
                + [p.loss[f] for f in CSV_LOSS_FIELDS]
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

    baselines = compute_baselines(points, bench)
    annotate(
        points,
        baselines,
        reference_branch_times(points, baselines),
        reference_bytes_per_atom(points, baselines),
    )

    # Imported here: reporter imports back from this module.
    from tracecliff import reporter

    reporter.report(points, bench, path.name)

    if csv_out:
        ANALYZED_DIR_DEFAULT.mkdir(parents=True, exist_ok=True)
        write_csv_out(points, bench, ANALYZED_DIR_DEFAULT / f"analyzed_{path.stem}.csv")

    return bench, points


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


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    add_arguments(parser)
    run(parser.parse_args(argv))
