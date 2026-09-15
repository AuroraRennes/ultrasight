"""
reporter.py — print one analyzed sweep as a text report
"""

import statistics
from collections import Counter
from itertools import product

from tracecliff.analyzer import REF_NO_ANALYTIC, best_run, by_workload, true_count
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
    row_axis, col_axis, iters_axis = bench.axes
    rows, cols = axes[row_axis.field], axes[col_axis.field]
    max_iters = max(axes[iters_axis.field])

    print(f"\n{'=' * 78}\n{fname}  ({bench.name}: {row_axis.field} x {col_axis.field})")
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
                p = by_cell.get(((r, c, max_iters), arms_key))
                cells[r, c] = fmt_pct(p.loss.loss_pct if p else None)
        label = " ".join(f"{name}={v}" for name, v in chosen.items()) or "all points"
        print_grid(
            f"-- {label}: loss% at iters={max_iters:,} --",
            rows,
            cols,
            cells,
            row_axis.field,
            col_axis.field,
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
