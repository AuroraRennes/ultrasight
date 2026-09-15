"""
analyzer.py — compute CoreSight ETM overflow-sweep results.

    tracecliff analyze [files...] [--csv-out]
"""

import argparse
import csv
import sys
from collections import defaultdict
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
RESULTS_DIR = REPO_ROOT / "results"
ANALYZED_DIR_DEFAULT = RESULTS_DIR / "analyzed_results"

# Averaged over the repetitions of each point. child_time_s is the clock the
# rates are computed against; instr_/global_time_s are unused.
NUMERIC_FIELDS = ["child_time_s", "overflow_count", "raw_trace_bytes", "atom_elem_sum"]

# Ground truth. edges_total is NOT usable for addr: where the ETM's address
# cache half-hits it undercounts by 3-10% while atoms stay exact.
TRUE_METRIC = "atom_elem_sum"

# bench_addr.c: every executed branch costs exactly 2 atom elements, so the
# analytic count is 2 x branches.
ATOMS_PER_BRANCH = 2
ADDR_LOOP_UNROLL = 8  # iterations per unrolled block
ADDR_LOOP_INDIRECT_PER_ITER = 2  # the indirect call and its return
# A lap of N hops is the BL to t0, N-1 BRs and the final RET (N+1 branches),
# plus the loop branch in bench() that starts the next lap.
ADDR_CHAIN_LAP_OVERHEAD_BRANCHES = 2

# Shortest run whose bytes/atom is trustworthy: below it the fixed trace-enable
# preamble is a visible fraction of raw_trace_bytes.
COST_MIN_ITERS = 100_000


# A run is lossless when it lands within this much of the analytic count.
LOSSLESS_TOL = 0.02
LOSSLESS_ABS_SLACK = 256

# Per-kind CSV column names and table labels.
AXIS_FIELDS = {"addr": ("n_targets", "spacing")}
AXIS_LABELS = {"addr": ("N", "stride S")}

# Arm factors the report splits on. A sweep writes only the factors it varied,
# so an absent column means "held fixed" and any constant serves as its key.
ARM_FIELDS = ("etr", "bb")

# Older sweeps named the overflow counter differently and kept a full path.
LEGACY_COLUMNS = {
    "etr_overflow_count": "overflow_count",
    "etm_overflow_count": "overflow_count",
    "binary": "binary_name",
}


# ---------------------------------------------------------------------------
# Analytic ground truth
# ---------------------------------------------------------------------------


def slack(count):
    """Tolerance around count"""
    return max(count * LOSSLESS_TOL, LOSSLESS_ABS_SLACK) if count else 0


def is_lossless(count, base):
    return count >= base - slack(base)


def analytic_count(kind, variant, axes, iters):
    """Atom elements a lossless trace of this point decodes, or None without a formula"""
    if kind == "addr":
        n_targets = axes[0]
        if variant == "chain":
            # The chain only runs whole laps of N hops: iters rounds up to one.
            laps = -(-iters // n_targets)  # ceil
            branches = laps * (n_targets + ADDR_CHAIN_LAP_OVERHEAD_BRANCHES)
        else:
            # One loop-back branch per block of ADDR_LOOP_UNROLL calls, then one
            # per call in the tail loop that runs the remainder.
            blocks, tail = divmod(iters, ADDR_LOOP_UNROLL)
            branches = iters * ADDR_LOOP_INDIRECT_PER_ITER + blocks + tail
        return branches * ATOMS_PER_BRANCH

    return None


# ---------------------------------------------------------------------------
# Loading / grouping
# ---------------------------------------------------------------------------


def to_float(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def load_rows(path):
    """Rows from a sweep CSV, with legacy column names normalised."""
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    for r in rows:
        for old, new in LEGACY_COLUMNS.items():
            if old in r and new not in r:
                r[new] = r.pop(old)
    return rows


def detect_kind(fieldnames):
    if "n_targets" in fieldnames and "spacing" in fieldnames:
        return "addr"
    return None


def detect_variant(rows):
    """'chain' or 'loop' for an addr sweep, from the binary_name column."""
    for r in rows:
        base = (r.get("binary_name") or "").rsplit("/", 1)[-1]
        if base.startswith("bench_chain_"):
            return "chain"
        if base.startswith("bench_"):
            return "loop"
    return None


def fixed_factors(rows):
    """The arm factors absent from these rows, i.e. fixed rather than swept."""
    return tuple(f for f in ARM_FIELDS if f not in rows[0]) if rows else ()


def point_key(row, kind):
    """(row axis, col axis, iters, etr, bb) — the identity of one swept point."""
    fields = AXIS_FIELDS[kind] + ("iters",)
    return tuple(int(row[f]) for f in fields) + tuple(
        int(row[f]) if f in row else 0 for f in ARM_FIELDS
    )


def workload_key(key):
    """One workload, both arms stripped: TRUE_METRIC is etr- and bb-invariant."""
    *axes, iters, _etr, _bb = key
    return (tuple(axes), iters)


def group_points(rows, kind):
    """Average the repetitions at each (axis..., iters, etr, bb) point."""
    groups = defaultdict(list)
    for r in rows:
        groups[point_key(r, kind)].append(r)

    points = {}
    for key, grp in groups.items():
        agg = {"n_runs": len(grp)}
        for f in NUMERIC_FIELDS:
            vals = [v for v in (to_float(r.get(f)) for r in grp) if v is not None]
            agg[f] = sum(vals) / len(vals) if vals else None
            if f == TRUE_METRIC:
                agg["n_ok"] = len(vals)
        agg["true_count"] = agg[TRUE_METRIC]
        agg["baseline_ref"] = None  # filled in by compute_baselines
        points[key] = agg
    return points


def by_workload(points):
    """Points that measured anything, grouped under their workload key."""
    out = defaultdict(list)
    for key, agg in points.items():
        if agg["true_count"] is not None:
            out[workload_key(key)].append(agg)
    return out


def best_run(aggs):
    """The run a workload's baseline leans on, and whether it overflowed."""
    best = max(aggs, key=lambda a: a["true_count"])
    return best, best.get("overflow_count") or 0


def axis_values(points):
    """The five key components, each sorted: rows, cols, iters, etrs, bbs."""
    return tuple(sorted({k[i] for k in points}) for i in range(5))


# ---------------------------------------------------------------------------
# Baseline / loss computation
# ---------------------------------------------------------------------------


# Provenance of a baseline read from the runs because no formula covers the
# workload: the one case where an overflowed best run understates the reference.
REF_NO_ANALYTIC = "measured-max (no analytic count for this config)"


def compute_baselines(points, kind, variant=None):
    """Per-workload lossless reference counts, tagging provenance on each point.
    Analytic where derivable; measured runs only corroborate it."""
    baselines = {}
    for wkey, aggs in by_workload(points).items():
        axes, iters = wkey
        expected = analytic_count(kind, variant, axes, iters)
        best, best_ovf = best_run(aggs)
        observed = best["true_count"]

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
        for agg in aggs:
            agg["baseline_ref"] = ref

    return baselines


def reference_bytes_per_atom(points, baselines):
    """Trace bytes per atom, per (axes, bb), from that key's longest clean runs.
    Short runs inflate it (preamble), lossy runs understate it."""
    per_key = defaultdict(list)
    for key, agg in points.items():
        *axes, iters, _etr, bb = key
        base = baselines.get(workload_key(key))
        raw, true = agg.get("raw_trace_bytes"), agg.get("true_count")
        if agg.get("overflow_count") or not base or not raw or not true:
            continue
        if iters < COST_MIN_ITERS or not is_lossless(true, base):
            continue
        per_key[(tuple(axes), bb)].append((iters, raw / true))

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
    for key, agg in points.items():
        wkey = workload_key(key)
        base = baselines.get(wkey)
        secs, count = agg.get("child_time_s"), agg["true_count"]
        if not base or not secs or not count or not is_lossless(count, base):
            continue
        per_branch = secs / base
        if wkey not in best or per_branch < best[wkey]:
            best[wkey] = per_branch
    return best


# Every key annotate() derives, so a point that derives nothing still carries
# the full set (the CSV writer reads them all).
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
    for key, agg in points.items():
        *axes, _, _, bb = key
        wkey = workload_key(key)
        base = baselines.get(wkey)
        agg.update(DERIVED_DEFAULTS)
        agg["baseline_edges"] = base
        if not base or agg["true_count"] is None:
            continue

        count, secs = agg["true_count"], agg.get("child_time_s")
        agg["loss_pct"] = max(0.0, (base - count) / base * 100.0)
        if secs:
            # offered = what the workload generated, delivered = what came back,
            # bytes_per_s = what the sink moved (the quantity ETR limits).
            agg["offered_edge_rate"] = base / secs
            agg["delivered_edge_rate"] = count / secs
            if agg.get("raw_trace_bytes"):
                agg["bytes_per_s"] = agg["raw_trace_bytes"] / secs
            # An estimate on lossy points: the baseline atom count priced at
            # the packet cost of this workload's own clean runs.
            bpa = ref_bytes_per_atom.get((tuple(axes), bb))
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
        n_ovf = agg.get("overflow_count")
        # Meaningless where true_count is a framing artefact rather than a count.
        if n_ovf and missing and ref and not agg["inflated"]:
            agg["escape_window_s"] = (missing / n_ovf) * ref


CSV_AGG_FIELDS = [
    "n_runs",
    "n_ok",
    "atom_elem_sum",
    "true_count",
    "overflow_count",
    "child_time_s",
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


def write_csv_out(points, kind, out_path):
    key_fields = AXIS_FIELDS[kind] + ("iters", "etr", "bb")
    with out_path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(list(key_fields) + CSV_AGG_FIELDS)
        for key in sorted(points):
            w.writerow(list(key) + [points[key][f] for f in CSV_AGG_FIELDS])
    print(f"  wrote {out_path}")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def analyze_file(path, csv_out):
    rows = load_rows(path)
    if not rows:
        print(f"\n{path}: empty, skipping")
        return None
    kind = detect_kind(rows[0].keys())
    if kind is None:
        print(f"\n{path}: unrecognized schema (no n_targets/spacing columns), skipping")
        return None

    # Not a warning: a sweep may hold factors fixed. Printed so the arms below
    # are not read as covering a grid the sweep never walked.
    fixed = fixed_factors(rows)
    if fixed:
        print(
            f"\n{path.name}: {', '.join(fixed)} not swept "
            "(fixed for this sweep) — arms below collapse to one level"
        )

    points = group_points(rows, kind)
    baselines = compute_baselines(points, kind, detect_variant(rows))
    annotate(
        points,
        baselines,
        reference_branch_times(points, baselines),
        reference_bytes_per_atom(points, baselines),
    )

    if csv_out:
        ANALYZED_DIR_DEFAULT.mkdir(parents=True, exist_ok=True)
        write_csv_out(points, kind, ANALYZED_DIR_DEFAULT / f"analyzed_{path.stem}.csv")

    return kind, points


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
