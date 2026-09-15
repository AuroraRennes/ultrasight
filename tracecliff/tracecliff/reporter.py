"""
reporter.py — print one analyzed sweep as a text report
"""

import statistics
from collections import Counter

from tracecliff.analyzer import (
    AXIS_FIELDS,
    AXIS_LABELS,
    REF_NO_ANALYTIC,
    axis_values,
    best_run,
    by_workload,
    workload_key,
)

MAX_TAINTED_SHOWN = 10


# Per-kind banner heading each file's report.
KIND_BANNER = {"addr": "kind=addr / bench_addr: N_TARGETS x STUB_STRIDE"}


def fmt_pct(v):
    return "   -  " if v is None else f"{v:5.1f}%"


def provenance_summary(points):
    """Baseline provenance counted per workload, not per point."""
    per_workload = {workload_key(k): a["baseline_ref"] for k, a in points.items()}
    counts = Counter(v for v in per_workload.values() if v is not None)
    return ", ".join(f"{v} ({n} workloads)" for v, n in sorted(counts.items()))


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
        (agg["offered_edge_rate"], agg["loss_pct"])
        for agg in points.values()
        if agg["offered_edge_rate"] is not None and agg["loss_pct"] is not None
    )
    if not rows:
        return
    print("\n-- edge-rate view (all N/stride/iters/etr/bb points) --")
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


KIND_TAIL = {"addr": edge_rate_view}


def sink_throughput(points):
    """Bytes/s moved on the saturated points: where a flat hardware limit shows."""
    vals = sorted(
        agg["bytes_per_s"]
        for agg in points.values()
        if agg.get("bytes_per_s") and (agg.get("loss_pct") or 0) >= 0.5
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
    vals = [(key, agg) for key, agg in points.items() if agg.get("escape_window_s")]
    print("\n-- escape window (Delta-t) --")
    if not vals:
        ovf = sum(1 for a in points.values() if a.get("overflow_count"))
        print(
            f"  not measurable: {ovf} point(s) recorded an overflow, "
            f"0 with a usable branch deficit"
        )
        return

    ws = sorted(a["escape_window_s"] for _, a in vals)
    biased = sum(1 for _, a in vals if a["escape_window_ref"] != "lossless-arm")
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


def report(points, kind, fname):
    """Everything printed for one file: axes, provenance, loss% grids, warnings."""
    rows, cols, iterss, etrs, bbs = axis_values(points)
    max_iters = max(iterss)
    row_name, col_name = AXIS_FIELDS[kind]
    row_label, col_label = AXIS_LABELS[kind]

    print(f"\n{'=' * 78}\n{fname}  ({KIND_BANNER[kind]})")
    for name, values in (
        (row_name, rows),
        (col_name, cols),
        ("iters", iterss),
        ("etr", etrs),
        ("bb", bbs),
    ):
        print(f"{name} values: {values}")
    print(f"baseline provenance: {provenance_summary(points)}")

    for etr in etrs:
        for bb in bbs:
            cells = {}
            for r in rows:
                for c in cols:
                    agg = points.get((r, c, max_iters, etr, bb))
                    cells[r, c] = fmt_pct(agg["loss_pct"] if agg else None)
            print_grid(
                f"-- etr={etr} bb={bb}: loss% at iters={max_iters:,} --",
                rows,
                cols,
                cells,
                row_label,
                col_label,
            )

    KIND_TAIL[kind](points)
    sink_throughput(points)
    escape_window_report(points)
    etr_bb_comparison(points)
    tainted_report(points)
    inflated_report(points, kind)


def etr_bb_comparison(points):
    """Mean/min/max loss% at the largest common iters, split by etr then by bb."""
    max_iters = max(k[2] for k in points)

    for name, idx, header in (
        ("etr", 3, "\n-- ETR=0 vs ETR=1 --"),
        ("bb", 4, "\n-- branch-broadcast on (bb=1) vs off (bb=0) --"),
    ):
        values = sorted({k[idx] for k in points})
        if len(values) < 2:
            continue
        print(header)
        for v in values:
            losses = [
                agg["loss_pct"]
                for key, agg in points.items()
                if key[2] == max_iters and key[idx] == v and agg["loss_pct"] is not None
            ]
            if losses:
                print(
                    f"  {name}={v}: mean loss {sum(losses) / len(losses):5.1f}%  "
                    f"min {min(losses):5.1f}%  max {max(losses):5.1f}%  "
                    f"(n={len(losses)} points @ iters={max_iters:,})"
                )


def inflated_report(points, kind):
    """Points reading as 0.0% loss that are decoder over-counts, not clean runs."""
    axis_names = AXIS_FIELDS[kind]
    bad = [(key, agg) for key, agg in points.items() if agg.get("inflated")]
    if not bad:
        return
    print(
        f"\n-- WARNING: {len(bad)} point(s) with true_count > baseline "
        f"(reported as 0.0% loss above, but not actually clean) --"
    )
    for key, agg in sorted(bad):
        a, b, iters, etr, bb = key
        print(
            f"  {axis_names[0]}={a} {axis_names[1]}={b} iters={iters:,} etr={etr} bb={bb}: "
            f"true_count={agg['true_count']:,.0f}  baseline={agg['baseline_edges']:,.0f}  "
            f"overflow_count={agg.get('overflow_count')}"
        )


def tainted_report(points):
    """Warn where a formula-less baseline came from a run that overflowed,
    since loss% against it then understates the loss."""
    tainted = []
    for wkey, aggs in by_workload(points).items():
        best, best_ovf = best_run(aggs)
        if best_ovf > 0 and best["baseline_ref"] == REF_NO_ANALYTIC:
            tainted.append((wkey, best["true_count"], best_ovf))
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
    for wkey, observed, ovf in sorted(tainted, key=lambda t: -t[2])[:MAX_TAINTED_SHOWN]:
        print(f"       {wkey}  baseline={observed:,.0f}  overflow_count={ovf:,.0f}")
    if len(tainted) > MAX_TAINTED_SHOWN:
        print(f"       ... and {len(tainted) - MAX_TAINTED_SHOWN} more")
