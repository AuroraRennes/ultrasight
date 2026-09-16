"""
reporter.py — print one analyzed sweep as a text report
"""

import statistics
from collections import Counter
from itertools import product

from tracecliff.analyzer import (
    OUTCOME_FIELD,
    REF_NO_ANALYTIC,
    REFERENCE_ARMS,
    REFERENCE_BINARY,
    best_run,
    branches,
    by_workload,
    delivered,
    find_reference,
    true_count,
)
from tracecliff.points import arm_values, axis_values

MAX_TAINTED_SHOWN = 10


def fmt_pct(v):
    return "   -  " if v is None else f"{v:5.1f}%"


def provenance_summary(points):
    """Baseline provenance counted per workload, not per point."""
    per_workload = {k.workload: p.loss.baseline_ref for k, p in points.items()}
    counts = Counter(v for v in per_workload.values() if v is not None)
    return ", ".join(f"{v} ({n} workloads)" for v, n in sorted(counts.items()))


def describe(key, bench):
    """ "n_targets=8 spacing=64 iters=1,000,000 etr=0" for one point."""
    axes = [f"{a.field}={v:,}" for a, v in zip(bench.axes, key.axes)]
    return " ".join(axes + [f"{name}={v}" for name, v in key.arms])


def print_grid(title, row_vals, col_vals, cells, row_label, col_label):
    """Print `cells`, a {(row, col): text} dict, as a table."""
    print(f"\n{title}")
    header = f"{row_label:>6} | " + " | ".join(f"{c:>7}" for c in col_vals)
    print(header)
    print("-" * len(header))
    for r in row_vals:
        row = " | ".join(f"{cells[r, c]:>7}" for c in col_vals)
        print(f"{r:>6} | {row}")
    print(f"(columns: {col_label})")


def edge_rate_view(points):
    """Whether zero-loss and lossy points separate cleanly by edge rate."""
    rows = sorted(
        (p.loss.offered_edge_rate, p.loss.loss_pct)
        for p in points.values()
        if p.loss.offered_edge_rate is not None and p.loss.loss_pct is not None
    )
    if not rows:
        return
    print("\n-- edge-rate view (all points) --")
    zero = [r for r in rows if r[1] < 0.5]
    lossy = [r for r in rows if r[1] >= 0.5]
    if zero:
        print(
            f"  zero-loss (<0.5%) offered rates: "
            f"{zero[0][0]:,.0f} .. {zero[-1][0]:,.0f} /s  ({len(zero)} points)"
        )
    if lossy:
        print(
            f"  lossy (>=0.5%) offered rates:    "
            f"{lossy[0][0]:,.0f} .. {lossy[-1][0]:,.0f} /s  ({len(lossy)} points)"
        )
    if zero and lossy:
        gap_lo, gap_hi = zero[-1][0], lossy[0][0]
        if gap_hi > gap_lo:
            print(
                f"  => observed safe floor: <= {gap_lo:,.0f} edges/s never lost data "
                f"in this run; first loss seen at {gap_hi:,.0f} edges/s"
            )
        else:
            print(
                "  => zero-loss and lossy edge-rate ranges overlap in this run "
                "(packet-type cost, not rate alone, decides outcome — see per-N table above)"
            )


def sink_throughput(points):
    """Bytes/s moved on the saturated points: where a flat hardware limit shows."""
    vals = sorted(
        p.loss.bytes_per_s
        for p in points.values()
        if p.loss.bytes_per_s and (p.loss.loss_pct or 0) >= 0.5
    )
    if not vals:
        return
    mid = statistics.median_high(vals)
    print("\n-- sink throughput on lossy points --")
    print(
        f"  median {mid / 1e6:,.1f} MB/s   range {vals[0] / 1e6:,.1f} .. {vals[-1] / 1e6:,.1f} MB/s"
        f"  ({len(vals)} points)"
    )


def escape_window_report(points):
    """ARMOR's Delta-t: trace-blind time per overflow event, where measurable"""
    losses = [p.loss for p in points.values() if p.loss.escape_window_s]
    print("\n-- escape window (Delta-t) --")
    if not losses:
        ovf = sum(1 for p in points.values() if p.mean("overflow_count"))
        print(
            f"  not measurable: {ovf} point(s) recorded an overflow, "
            f"0 with a usable branch deficit"
        )
        return

    ws = sorted(loss.escape_window_s for loss in losses)
    biased = sum(1 for loss in losses if loss.escape_window_ref != "lossless-arm")
    mid = statistics.median_high(ws)
    print(
        f"  median {mid * 1e6:,.1f} us   range {ws[0] * 1e6:,.1f} .. "
        f"{ws[-1] * 1e6:,.1f} us  ({len(ws)} points)"
    )
    print("  (ARMOR report 172.4 us for the 64 KB ETF on ARM Juno R2, measured")
    print("   at the ETF)")
    if biased:
        print(
            f"  WARNING: {biased} point(s) had no lossless reference run; "
            f"their branch rate is stall-biased and Delta-t is overstated"
        )


def report(points, bench, fname):
    """Everything printed for one file: axes, provenance, loss% grids, warnings."""
    axes = axis_values(points, bench)
    arms = arm_values(points)
    varying = {name: values for name, values in arms.items() if len(values) > 1}
    fixed = [f"{name}={values[0]}" for name, values in arms.items() if len(values) == 1]
    # A two-axis bench prints a grid; a one-axis bench a single loss% column.
    *grid_axes, iters_axis = bench.axes
    row_axis = grid_axes[0]
    rows = axes[row_axis.field]
    if len(grid_axes) == 2:
        col_label = grid_axes[1].field
        cols = axes[col_label]
    else:
        col_label, cols = "loss%", ["loss%"]
    max_iters = max(axes[iters_axis.field])

    grid_names = " x ".join(a.field for a in grid_axes)
    print(f"\n{'=' * 78}\n{fname}  ({bench.name}: {grid_names})")
    for name, values in axes.items():
        print(f"{name} values: {values}")
    for name, values in varying.items():
        print(f"{name} values: {values}")
    if fixed:
        print(f"fixed: {' '.join(fixed)}")
    print(f"baseline provenance: {provenance_summary(points)}")

    by_cell = {(k.axes, k.arms): p for k, p in points.items()}
    for combo in product(*varying.values()):
        chosen = dict(zip(varying, combo))
        arms_key = tuple(
            (name, chosen.get(name, values[0])) for name, values in arms.items()
        )
        cells = {}
        for r in rows:
            for c in cols:
                point_axes = (
                    (r, c, max_iters) if len(grid_axes) == 2 else (r, max_iters)
                )
                p = by_cell.get((point_axes, arms_key))
                cells[r, c] = fmt_pct(p.loss.loss_pct if p else None)
        label = " ".join(f"{name}={v}" for name, v in chosen.items()) or "all points"
        print_grid(
            f"-- {label}: loss% at iters={max_iters:,} --",
            rows,
            cols,
            cells,
            row_axis.field,
            col_label,
        )

    edge_rate_view(points)
    sink_throughput(points)
    escape_window_report(points)
    arm_comparison(points, varying)
    tainted_report(points, bench)
    inflated_report(points, bench)


def arm_comparison(points, varying):
    """Mean/min/max loss% at the largest iters, per value of each varying factor."""
    max_iters = max(k.iters for k in points)

    for name, values in varying.items():
        print(f"\n-- {' vs '.join(f'{name}={v}' for v in values)} --")
        for v in values:
            losses = [
                p.loss.loss_pct
                for key, p in points.items()
                if key.iters == max_iters
                and key.arm(name) == v
                and p.loss.loss_pct is not None
            ]
            if losses:
                print(
                    f"  {name}={v}: mean loss {sum(losses) / len(losses):5.1f}%  "
                    f"min {min(losses):5.1f}%  max {max(losses):5.1f}%  "
                    f"(n={len(losses)} points @ iters={max_iters:,})"
                )


def inflated_report(points, bench):
    """Points reading as 0.0% loss that are decoder over-counts, not clean runs."""
    bad = [(key, p) for key, p in points.items() if p.loss.inflated]
    if not bad:
        return
    print(
        f"\n-- WARNING: {len(bad)} point(s) with true_count > baseline "
        f"(reported as 0.0% loss above, but not actually clean) --"
    )
    for key, p in sorted(bad, key=lambda kp: kp[0]):
        print(
            f"  {describe(key, bench)}: "
            f"true_count={true_count(p):,.0f}  baseline={p.loss.baseline_edges:,.0f}  "
            f"overflow_count={p.mean('overflow_count')}"
        )


def tainted_report(points, bench):
    """Warn where a formula-less baseline came from a run that overflowed,
    since loss% against it then understates the loss."""
    tainted = []
    for ps in by_workload(points).values():
        best, best_ovf = best_run(ps)
        if best_ovf > 0 and best.loss.baseline_ref == REF_NO_ANALYTIC:
            tainted.append((best.key, true_count(best), best_ovf))
    if not tainted:
        return

    print(
        f"\n  !! {len(tainted)} workload(s) took a MEASURED baseline from a run "
        f"that emitted ETM overflow packets."
    )
    print(
        "     That baseline is below the true count, so every loss% against "
        "it understates the loss."
    )
    for key, observed, ovf in sorted(tainted, key=lambda t: -t[2])[:MAX_TAINTED_SHOWN]:
        print(
            f"       {describe(key, bench)}  baseline={observed:,.0f}  "
            f"overflow_count={ovf:,.0f}"
        )
    if len(tainted) > MAX_TAINTED_SHOWN:
        print(f"       ... and {len(tainted) - MAX_TAINTED_SHOWN} more")


# ---------------------------------------------------------------------------
# Per-arm report
# ---------------------------------------------------------------------------

# Column order. overflow_count is the only outcome; the rest explain it, and
# never stand in for it.
OUTCOME = OUTCOME_FIELD
COVARIATES = ["child_time_s", "raw_trace_bytes", "atom_elem_sum"]
# Derived, not a CSV column: the share of the expected atom stream that arrived.
# The guard every stage requires, since back-pressure relocates loss rather
# than removing it.
DELIVERED = "delivered"
ARM_METRICS = [OUTCOME] + COVARIATES + [DELIVERED]


def fmt_value(v, width=12):
    if v is None:
        return "ERR".rjust(width)
    if v == 0:
        return "0".rjust(width)
    if abs(v) >= 1e6 or (abs(v) < 1e-3):
        return f"{v:.3e}".rjust(width)
    if abs(v) >= 100:
        return f"{v:,.0f}".rjust(width)
    return f"{v:.4f}".rjust(width)


def fmt_delta(point_v, ref_v, width=14):
    """A ratio, or an absolute where the reference is zero."""
    if point_v is None or ref_v is None:
        return "-".rjust(width)
    if ref_v == 0:
        return "=0".rjust(width) if point_v == 0 else f"+{point_v:.4g}".rjust(width)
    return f"x{point_v / ref_v:.3f}".rjust(width)


def arm_spread(point, metric):
    """Rep-to-rep sd of one metric, 0 for values derived from a mean."""
    if metric == DELIVERED:
        return 0.0
    vals = point.samples.get(metric) or []
    return statistics.stdev(vals) if len(vals) > 1 else 0.0


def arm_value(point, metric, per_branch=False):
    """One metric, optionally per branch"""
    if metric == DELIVERED:
        return delivered(point)
    v = point.mean(metric)
    if v is None or not per_branch:
        return v
    br = branches(point)
    return v / br if br else None


def arm_label(key):
    return " ".join(f"{name}={v}" for name, v in key.arms) or "(defaults)"


def workload_label(key, bench):
    return " ".join(f"{a.field}={v}" for a, v in zip(bench.axes, key.axes))


def report_arms(points, bench, fname, per_branch=False, reference=None):
    """Every point as a difference from its workload's reference arm."""
    reference = reference or REFERENCE_ARMS
    arm_fields = [name for name, _v in next(iter(points)).arms]
    per_workload, global_ref = find_reference(points, reference)

    print(f"\n{'=' * 78}\n{fname}  ({bench.name})")
    print(f"axes    : {', '.join(a.field for a in bench.axes)}")
    print(f"factors : {', '.join(arm_fields) or '(none)'}")
    print(f"points  : {len(points)}  ({sum(p.n_runs for p in points.values())} runs)")
    ref_desc = " ".join(f"{k}={v}" for k, v in reference.items() if k in arm_fields)
    print(f"reference level: {ref_desc or '(no swept factor is at a reference level)'}")
    if global_ref is not None:
        print(
            f"reference point present: {REFERENCE_BINARY} "
            f"({global_ref.n_runs} reps, "
            f"{OUTCOME}={fmt_value(global_ref.mean(OUTCOME)).strip()}, "
            f"sd={fmt_value(arm_spread(global_ref, OUTCOME)).strip()})"
        )
    if not per_workload:
        print(
            "WARNING: no reference point in this CSV — the sweep never visited "
            "the reference level, so every difference below would be against "
            "nothing. Reporting absolute values only."
        )

    unit = "/branch" if per_branch else ""

    def col(m):
        """`delivered` is a ratio, so it never takes the /branch suffix."""
        return m if m == DELIVERED else m + unit

    # Widened to the data, so a long label cannot eat the column beside it.
    wl_w = max([len(workload_label(k, bench)) for k in points] + [len("workload")]) + 2
    fc_w = max([len(arm_label(k)) for k in points] + [len("factors")]) + 2
    # Headers are wider than any value once "/branch" is appended.
    mw = max([len(col(m)) for m in ARM_METRICS] + [len(OUTCOME) + 3]) + 2
    header = (
        f"  {'workload':<{wl_w}}{'factors':<{fc_w}}"
        + "".join(f"{col(m):>{mw}}" for m in ARM_METRICS)
        + f"{'d(' + OUTCOME + ')':>{mw}}"
    )
    print("\n" + header)
    print("  " + "-" * (len(header) - 2))

    # Workloads in CSV order, reference arm first within each.
    for axes in dict.fromkeys(k.axes for k in points):
        group = [(k, p) for k, p in points.items() if k.axes == axes]
        ref = per_workload.get(axes)
        group.sort(key=lambda kp: (kp[1] is not ref, arm_label(kp[0])))
        for key, point in group:
            is_ref = point is ref
            vals = "".join(
                fmt_value(arm_value(point, m, per_branch), mw) for m in ARM_METRICS
            )
            delta = (
                "reference".rjust(mw)
                if is_ref
                else fmt_delta(
                    arm_value(point, OUTCOME, per_branch),
                    arm_value(ref, OUTCOME, per_branch) if ref else None,
                    mw,
                )
            )
            print(
                f" {'*' if is_ref else ' '}{workload_label(key, bench):<{wl_w}}"
                f"{arm_label(key):<{fc_w}}{vals}{delta}"
            )
        print()

    arm_spread_note(per_workload, bench, wl_w)


def arm_spread_note(per_workload, bench, wl_w):
    """The reference arms' rep-to-rep spread: the resolution floor every
    difference above has to clear to be a result."""
    if not per_workload:
        return
    print("  reference spread (rep-to-rep sd, the resolution floor):")
    for point in per_workload.values():
        if point.n_runs < 2:
            continue
        parts = []
        for m in ARM_METRICS:
            mean, sd = arm_value(point, m), arm_spread(point, m)
            rel = f" ({sd / mean * 100:.1f}%)" if mean else ""
            parts.append(f"{m}={fmt_value(sd).strip()}{rel}")
        print(
            f"    {workload_label(point.key, bench):<{wl_w}}"
            f"n={point.n_runs}  " + "  ".join(parts)
        )
