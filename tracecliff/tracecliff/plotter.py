"""
plotter.py — the campaign figures, drawn from pooled sweep CSVs at bb=1.

    cliff     loss, slowdown and rate delivered against the rate demanded
    filters   call_fill trace demand against call_k under each address filter
    corpus    a fuzzing corpus replayed: each input's overflow packets, per stall

Port load is trace bytes per second over what the TPIU port can carry. A cell's
offered load is its overflow-free byte count over its stall=off run time.
matplotlib is imported lazily so the text report works without it.
"""

import csv
import random
import sys
from collections import defaultdict
from pathlib import Path

from tracecliff.analyzer import REFERENCE_ARMS, RESULTS_DIR, attach_losses, load_arms
from tracecliff.benches import CALL_P
from tracecliff.points import PointKey
from tracecliff.sweep import FACTOR_FLAGS

SURFACE = "#fcfcfb"
INK, INK_MUTED, INK_FAINT = "#0b0b0b", "#52514e", "#8a8880"

# Categorical series colours, assigned in this fixed order and never cycled.
SERIES_COLORS = ["#eb6834", "#4a3aa7", "#1a9e5f", "#c2408a"]

# The TPIU port: one 32-bit word per cycle of its 250 MHz trace clock.
PORT_BYTES_PER_S = 4 * 250e6

SINKS = {"0": "TPIU (etr=0)", "1": "ETR (etr=1)"}
SINK_COLORS = dict(zip(SINKS, SERIES_COLORS))

# A colour is an address filter in every figure that shows one.
FILTER_COLORS = {"etm": SERIES_COLORS[2], "none": SERIES_COLORS[3]}
FILTER_LABELS = {
    "etm": "addrfilter=etm: libc filtered out by the ETM",
    "none": "addrfilter=none: libc traced too",
}

FAMILIES = {"addr_chain": "chain", "addr_loop": "loop", "call_fill": "call_fill"}
FAMILY_MARKERS = {"chain": "o", "loop": "s", "call_fill": "^"}

# Below this a run's rate averages in its fixed start-up and dilutes its loss.
CONVERGED_ITERS = 10_000_000


def _pyplot():
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("plots need matplotlib: pip install 'tracecliff[plots]'", file=sys.stderr)
        return None
    return plt


# Pooling
# ---------------------------------------------------------------------------


def normalised(key):
    """The key with every factor present, a column the CSV lacks read at its
    reference level, so the same run from files with different columns pools."""
    arms = dict(key.arms)
    return PointKey(
        key.axes, tuple((f, arms.get(f, REFERENCE_ARMS.get(f))) for f in FACTOR_FLAGS)
    )


def pool(loaded):
    """Every loaded CSV as one point set per bench; repeat sessions at a key
    merge into one point with more reps."""
    pooled = {}
    for bench, points in loaded:
        _, merged = pooled.setdefault(bench.name, (bench, {}))
        for key, point in points.items():
            key = normalised(key)
            if key in merged:
                dst = merged[key]
                dst.n_runs += point.n_runs
                for metric, values in point.samples.items():
                    dst.samples[metric].extend(values)
            else:
                point.key = key
                merged[key] = point
    # Losses were attached per file; the pooled points have different means.
    for bench, points in pooled.values():
        attach_losses(points, bench)
    return pooled


def workload_cells(pooled):
    """(family, axes, addrfilter, bbfilter) -> (etr, stall) -> point, at bb=1 and
    cs-trace's own ETR drain."""
    cells = defaultdict(dict)
    for name, family in FAMILIES.items():
        for key, point in pooled.get(name, (None, {}))[1].items():
            if key.arm("bb") != "1" or key.iters < CONVERGED_ITERS:
                continue
            if key.arm("axictl") is not None or key.arm("udmabuf") is not None:
                continue
            cell = (family, key.axes, key.arm("addrfilter"), key.arm("bbfilter"))
            cells[cell][(key.arm("etr"), key.arm("stall"))] = point
    return cells


# Per-cell statistics
# ---------------------------------------------------------------------------


def reps(point, metric):
    return point.samples.get(metric, [])


def median(values):
    values = sorted(values)
    return values[len(values) // 2] if values else None


def clean_bytes(by_arm):
    """The cell's lossless trace size: bytes of its overflow-free reps, any arm."""
    return median(
        b
        for point in by_arm.values()
        for b, ovf in zip(reps(point, "raw_trace_bytes"), reps(point, "overflow_count"))
        if ovf == 0
    )


def offered_load(by_arm, etr):
    """Port load the cell asks of the sink: lossless bytes over unstalled time."""
    off = by_arm.get((etr, "off"))
    secs = off and median(reps(off, "child_time_s"))
    size = clean_bytes(by_arm)
    return size / secs / PORT_BYTES_PER_S if secs and size else None


def delivered_load(point):
    """Port load a run got out of its sink: captured bytes over run time."""
    size = point and median(reps(point, "raw_trace_bytes"))
    secs = point and median(reps(point, "child_time_s"))
    return size / secs / PORT_BYTES_PER_S if size and secs else None


def slowdown(by_arm, etr, stall):
    """Run time at a stall level over the same cell's stall=off run time."""
    off, point = by_arm.get((etr, "off")), by_arm.get((etr, stall))
    base = off and median(reps(off, "child_time_s"))
    secs = point and median(reps(point, "child_time_s"))
    return secs / base if base and secs else None


def loss_pct(point):
    return point.loss.loss_pct if point and point.loss else None


def sink_ceilings(cells):
    """etr -> the port load each sink delivers once saturated: the median over
    the stall=off points where every rep overflowed."""
    delivered = defaultdict(list)
    for by_arm in cells.values():
        for (etr, stall), point in by_arm.items():
            ovf = reps(point, "overflow_count")
            if stall != "off" or not ovf or min(ovf) == 0:
                continue
            load = delivered_load(point)
            if load:
                delivered[etr].append(load)
    return {etr: median(loads) for etr, loads in delivered.items()}


# Drawing
# ---------------------------------------------------------------------------


def style(ax, ylabel=None, xlabel=None):
    if ylabel:
        ax.set_ylabel(ylabel, color=INK_MUTED, fontsize=9)
    if xlabel:
        ax.set_xlabel(xlabel, color=INK_MUTED, fontsize=9)
    ax.tick_params(colors=INK_MUTED, labelsize=8)
    ax.grid(True, color=INK_FAINT, alpha=0.25, linewidth=0.6)
    ax.set_axisbelow(True)
    ax.set_facecolor(SURFACE)
    for edge in ("top", "right"):
        ax.spines[edge].set_visible(False)
    for edge in ("left", "bottom"):
        ax.spines[edge].set_color(INK_FAINT)


def panel_title(ax, text):
    ax.set_title(text, color=INK, fontsize=10, loc="left")


def legend(ax, **kw):
    ax.legend(frameon=False, fontsize=8, labelcolor=INK_MUTED, **kw)


def save(fig, path, title, subtitle, bottom=0.0):
    height = fig.get_size_inches()[1]
    fig.suptitle(title, color=INK, fontsize=12, x=0.02, y=1 - 0.30 / height, ha="left")
    fig.text(0.02, 1 - 0.58 / height, subtitle, color=INK_MUTED, fontsize=8.5)
    fig.patch.set_facecolor(SURFACE)
    fig.tight_layout(rect=[0, bottom / height, 1, 1 - 0.75 / height])
    fig.savefig(path, dpi=160, facecolor=SURFACE)
    print(f"  wrote {path}")


# Figures
# ---------------------------------------------------------------------------


def fig_cliff(pooled, out_dir, plt):
    """Every synthetic cell against the rate it asks for, coloured by sink and
    shaped by family: stall=off above, stall=3 below, cost left, rate right."""
    cells = workload_cells(pooled)
    port = PORT_BYTES_PER_S / 1e6
    ceilings = {etr: load * port for etr, load in sink_ceilings(cells).items()}
    # etr -> family -> [(demand MB/s, loss %, slowdown, MB/s at off, MB/s at 3)]
    series = defaultdict(lambda: defaultdict(list))
    for (family, *_), by_arm in cells.items():
        for etr in SINKS:
            load, loss = offered_load(by_arm, etr), loss_pct(by_arm.get((etr, "off")))
            if load is None or loss is None:
                continue
            rates = [delivered_load(by_arm.get((etr, s))) for s in ("off", "3")]
            series[etr][family].append(
                (load * port, loss, slowdown(by_arm, etr, "3"),
                 *(r and r * port for r in rates))
            )
    if not series:
        return "no converged bb=1 cell with a stall=off run"

    fig, axes = plt.subplots(2, 2, figsize=(13.6, 8.0), sharex=True)
    (loss_ax, off_ax), (slow_ax, stall_ax) = axes
    panels = [loss_ax, slow_ax, off_ax, stall_ax]
    x_max = max(p[0] for fam in series.values() for pts in fam.values() for p in pts)
    for ax in (off_ax, stall_ax):
        ax.plot([0, x_max], [0, x_max], color=INK_FAINT, linewidth=1, zorder=1)
    for etr, color in SINK_COLORS.items():
        for family, pts in series[etr].items():
            for col, ax in enumerate(panels, start=1):
                kept = [(p[0], p[col]) for p in pts if p[col] is not None]
                if kept:
                    ax.scatter(*zip(*kept), s=30, color=color, edgecolors=SURFACE,
                               marker=FAMILY_MARKERS[family], linewidths=0.6, zorder=3)
        if etr not in series:
            continue
        loss_ax.plot([], [], color=color, marker="o", linestyle="", label=SINKS[etr])
        ceiling = ceilings.get(etr)
        if ceiling:
            for ax in (loss_ax, slow_ax):
                ax.axvline(ceiling, color=color, linewidth=1, linestyle=":")
            for ax in (off_ax, stall_ax):
                ax.axhline(ceiling, color=color, linewidth=1, linestyle=":")
            # What a lossless sink of this capacity costs: time stretched to fit.
            xs = [0, ceiling, max(x_max, ceiling)]
            slow_ax.plot(xs, [max(1.0, x / ceiling) for x in xs], color=color,
                         linewidth=1.2, linestyle="--", zorder=2)
    for family, marker in FAMILY_MARKERS.items():
        if any(family in fam for fam in series.values()):
            loss_ax.plot([], [], color=INK_MUTED, marker=marker, linestyle="",
                         label=family)
    loss_ax.plot([], [], color=INK_MUTED, linewidth=1, linestyle=":",
                 label="sink ceiling (saturated delivery)")
    slow_ax.plot([], [], color=INK_MUTED, linewidth=1.2, linestyle="--",
                 label="max(1, demand / ceiling)")
    off_ax.plot([], [], color=INK_FAINT, linewidth=1, label="delivered = demand")

    demand = "trace demand (MB/s): lossless trace bytes over stall=off run time"
    labels = [
        ("a  stall=off: share of the trace lost", "trace lost (%)"),
        ("b  stall=3: run time stretched", "run time (x stall=off)"),
        ("c  stall=off: rate the sink delivered", "trace delivered (MB/s)"),
        ("d  stall=3: rate the sink delivered", "trace delivered (MB/s)"),
    ]
    for ax, (title, ylabel) in zip(panels, labels):
        panel_title(ax, title)
        style(ax, ylabel, demand)
        ax.tick_params(labelbottom=True)
    loss_ax.set_ylim(bottom=0)
    y_max = max(v for fam in series.values() for pts in fam.values()
                for p in pts for v in p[3:] if v is not None)
    for ax in (off_ax, stall_ax):
        ax.set_ylim(0, 1.25 * y_max)
    legend(loss_ax, loc="upper left")
    legend(slow_ax, loc="upper left")
    legend(off_ax, loc="lower right")
    save(
        fig,
        out_dir / "cliff.png",
        "Past the sink ceiling, stall=off drops trace and stall=3 slows the core",
        f"chain, loop and call_fill pooled  ·  bb=1  ·  iters >= {CONVERGED_ITERS:,}  ·  "
        "one mark per cell and sink",
    )
    return None


def fig_filters(pooled, out_dir, plt):
    """call_fill at bbfilter=all: trace demand against call_k under each address
    filter, both sinks' loss zones above their ceilings."""
    cells = {c: a for c, a in workload_cells(pooled).items() if c[0] == "call_fill"}
    port = PORT_BYTES_PER_S / 1e6
    ceilings = sink_ceilings(workload_cells(pooled))
    # addrfilter -> [(call_k, demand MB/s)]
    series = defaultdict(list)
    for (_, axes_, addrfilter, bbfilter), by_arm in cells.items():
        if bbfilter != "all" or addrfilter not in FILTER_COLORS:
            continue
        # Demand is the workload's, not the sink's: the sinks' loads average.
        loads = {etr: offered_load(by_arm, etr) for etr in SINKS}
        loads = {etr: load for etr, load in loads.items() if load is not None}
        if loads:
            demand = sum(loads.values()) / len(loads) * port
            series[addrfilter].append((axes_[0], demand))
    if not series:
        return "no call_fill cell at bb=1, bbfilter=all with a stall=off run"

    fig, ax = plt.subplots(figsize=(7.6, 5.0))
    top = max(max(d for _, d in pts) for pts in series.values()) * 1.06
    # Each sink's loss zone starts at its ceiling; the zones stack upward.
    edges = sorted((ceilings[etr] * port, etr) for etr in SINKS if ceilings.get(etr))
    for i, (lo, etr) in enumerate(edges):
        ax.axhspan(lo, top, color=SINK_COLORS[etr], alpha=0.10, linewidth=0)
        ax.axhline(lo, color=SINK_COLORS[etr], linewidth=1, linestyle=":")
        losing = " and ".join(SINKS[e].split()[0] for _, e in edges[: i + 1])
        verb = "loses" if i == 0 else "lose"
        ax.annotate(f"{losing} {verb} trace", (0.01, lo), xycoords=("axes fraction", "data"),
                    xytext=(0, 4), textcoords="offset points",
                    color=SINK_COLORS[etr], fontsize=8)
    for addrfilter, color in FILTER_COLORS.items():
        pts = sorted(series.get(addrfilter, []), key=lambda p: p[0])
        if not pts:
            continue
        ks, demands = zip(*pts)
        ax.plot(ks, demands, color=color, marker="o", markersize=4, linewidth=1.8,
                label=FILTER_LABELS[addrfilter])
    style(ax, "trace demand (MB/s)",
          f"loop iterations that call into libc, out of every {CALL_P} (call_k)")
    ax.set_ylim(0, top)
    legend(ax, loc="lower right")
    save(
        fig,
        out_dir / "filters.png",
        "Each filter crossing costs more bytes than tracing the short callee",
        "call_fill  ·  bb=1  ·  bbfilter=all  ·  "
        "demand: lossless trace bytes over stall=off run time",
    )
    return None


# (addrfilter, bbfilter) -> label, in drawing order.
FILTER_ARMS = {
    ("etm", "all"): "addrfilter=etm",
    ("none", "all"): "none · bbfilter=all",
    ("none", "in"): "none · bbfilter=in",
}


def load_corpus(results_dir=RESULTS_DIR):
    """Every corpus replay pooled, as (target, addrfilter, bbfilter) ->
    (etr, stall) -> input -> (overflow packets, run time, bytes) of each run."""
    runs = sorted(results_dir.glob("targets_corpus_bb1_*.csv"))
    if not runs:
        return None
    rows = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
    for path in runs:
        with path.open() as f:
            for r in csv.DictReader(f):
                arm = (r["target"], r["addrfilter"], r["bbfilter"])
                rows[arm][(r["etr"], r["stall"])][r["input"]].append(
                    (float(r["overflow_count"]), float(r["child_time_s"]),
                     float(r["raw_trace_bytes"]))
                )
    return rows


def fig_corpus(_pooled, out_dir, plt):
    """Every corpus input's median overflow count per target and filter arm, one
    mark per input and sink: stall=off on the left, stall=3 on the right."""
    rows = load_corpus()
    if not rows:
        return "no targets_corpus_bb1 CSV in results/"
    stalls = {"off": "stall=off", "3": "stall=3"}

    def typical(arm, etr, stall):
        return [median(run[0] for run in runs) for runs in rows[arm][(etr, stall)].values()]

    def cost(arm, etr):
        """Median over inputs of the stall=3 run time against stall=off, in %."""
        off, on = rows[arm][(etr, "off")], rows[arm][(etr, "3")]
        ratios = [median(run[1] for run in on[i]) / median(run[1] for run in off[i])
                  for i in on if i in off]
        return (median(ratios) - 1) * 100 if ratios else None

    def size(arm):
        """input -> lossless trace bytes: the stall=3 median on the TPIU sink."""
        return {i: median(run[2] for run in runs)
                for i, runs in rows[arm][("0", "3")].items()}

    def overflowing(target):
        return sum(v > 0 for arm in rows if arm[0] == target
                   for etr in SINKS for v in typical(arm, etr, "off"))

    targets = sorted({arm[0] for arm in rows}, key=overflowing)
    x_max = max(v for arm in rows for etr in SINKS for s in stalls
                for v in typical(arm, etr, s))
    jitter = random.Random(0)
    fig, axes = plt.subplots(1, len(stalls) + 1, sharey=True,
                             figsize=(15.8, 1.8 + 1.5 * len(targets)),
                             gridspec_kw={"width_ratios": [1, 0.55, 0.45]})
    ticks, names, sizes = [], [], []
    for ax, (stall, title) in zip(axes, stalls.items()):
        for group, target in enumerate(targets):
            for i, (af, bf) in enumerate(FILTER_ARMS):
                arm, y = (target, af, bf), group + 0.3 * (1 - i)
                if arm not in rows:
                    continue
                if ax is axes[0]:
                    n = len(rows[arm][("0", "off")])
                    ticks.append(y)
                    names.append(f"{target} ({n} inputs)  ·  {FILTER_ARMS[af, bf]}"
                                 if i == 0 else FILTER_ARMS[af, bf])
                costs = []
                for j, etr in enumerate(SINKS):
                    xs = typical(arm, etr, stall)
                    if not xs:
                        continue
                    ys = [y + 0.06 * (1 - 2 * j) + jitter.uniform(-0.04, 0.04)
                          for _ in xs]
                    ax.scatter(xs, ys, s=14, color=SINK_COLORS[etr], alpha=0.55,
                               linewidths=0, zorder=3)
                    ax.annotate(f"{sum(x > 0 for x in xs)}/{len(xs)}",
                                (1.03, y + 0.06 * (1 - 2 * j)), va="center",
                                xycoords=("axes fraction", "data"),
                                color=SINK_COLORS[etr], fontsize=7.5)
                    costs.append(cost(arm, etr))
                if stall == "3" and None not in costs:
                    ax.annotate(" / ".join(f"{c:+.1f}" for c in costs), (1.36, y),
                                xycoords=("axes fraction", "data"), va="center",
                                color=INK_MUTED, fontsize=7.5)
                    got, etm = size(arm), size((target, "etm", "all"))
                    more = median(got[k] / etm[k] for k in got if k in etm)
                    sizes.append((y, more, median(got.values())))
            if group:
                ax.axhline(group - 0.5, color=INK_FAINT, linewidth=0.5, alpha=0.4)
        ax.annotate("inputs that\noverflow / total", (1.03, 1.0),
                    xycoords="axes fraction", va="bottom", color=INK_MUTED,
                    fontsize=7.5)
        if stall == "3":
            ax.annotate("run time vs\nstall=off, %\nTPIU / ETR", (1.36, 1.0),
                        xycoords="axes fraction", va="bottom", color=INK_MUTED,
                        fontsize=7.5)
        ax.set_xscale("symlog", linthresh=1)
        ax.set_xlim(-0.4, x_max * 3)
        ax.set_ylim(-0.6, len(targets) - 0.4)
        panel_title(ax, title)
        style(ax, None, "ETM overflow packets in the input's median run")
        ax.grid(axis="y", visible=False)
    ax = axes[-1]
    ax.barh([y for y, _, _ in sizes], [more for _, more, _ in sizes], height=0.17,
            color=INK_FAINT, alpha=0.6, linewidth=0, zorder=3)
    for y, more, got in sizes:
        ax.annotate(f"x{more:.2f}  ·  {got / 1e3:,.0f} KB", (more, y), xytext=(4, 0),
                    textcoords="offset points", va="center", color=INK_MUTED,
                    fontsize=7.5)
    ax.axvline(1, color=INK_FAINT, linewidth=0.8, zorder=2)
    ax.set_xlim(0, max(more for _, more, _ in sizes) * 2)
    ax.set_xticks(range(int(max(more for _, more, _ in sizes)) + 1))
    ax.xaxis.set_major_formatter(lambda v, _: f"x{v:g}")
    panel_title(ax, "trace per input")
    style(ax, None, "median, vs addrfilter=etm")
    for group in range(1, len(targets)):
        ax.axhline(group - 0.5, color=INK_FAINT, linewidth=0.5, alpha=0.4)
    ax.tick_params(axis="y", length=0)
    ax.grid(axis="y", visible=False)
    axes[0].set_yticks(ticks, names)
    for etr, name in SINKS.items():
        axes[0].scatter([], [], s=14, color=SINK_COLORS[etr], label=name)
    fig.legend(*axes[0].get_legend_handles_labels(), loc="lower center", ncols=2,
               frameon=False, fontsize=8, labelcolor=INK_MUTED)
    save(
        fig,
        out_dir / "corpus.png",
        "No filter arm is overflow-free on every target at stall=off; stall=3 nearly is",
        "bb=1  ·  a fuzzing campaign's minimised corpus replayed, one mark per input "
        "and sink  ·  median of each input's runs  ·  run time and trace size: median over inputs",
        bottom=0.4,
    )
    return None


FIGURES = {
    "cliff": fig_cliff,
    "filters": fig_filters,
    "corpus": fig_corpus,
}


# Dispatch
# ---------------------------------------------------------------------------


def add_arguments(parser):
    parser.add_argument(
        "figures",
        nargs="*",
        default=["all"],
        metavar="FIGURE",
        help=f"what to draw: {', '.join(FIGURES)} or all (default: all). Naming "
        "one the data cannot support is an error; 'all' skips it with a note",
    )
    parser.add_argument(
        "--csv",
        nargs="+",
        type=Path,
        metavar="FILE",
        help="sweep CSVs to pool (default: the bb=1 addr and call_fill CSVs in "
        f"{RESULTS_DIR})",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=Path("plots"),
        help="output directory (default: plots)",
    )


def default_csvs():
    return [
        f
        for family in FAMILIES
        for f in sorted(RESULTS_DIR.glob(f"{family}_*_bb1_*.csv"))
    ]


def run(args):
    files = args.csv or default_csvs()
    loaded = [got for got in map(load_arms, files) if got]
    if not loaded:
        print("ERROR: no reportable CSV to plot", file=sys.stderr)
        sys.exit(1)
    sys.exit(draw(args.figures, loaded, args.out))


def draw(names, loaded, out_dir):
    """Draw the named figures from every loaded CSV, pooled; a named figure the
    data cannot support is an error, 'all' skips it."""
    plt = _pyplot()
    if plt is None:
        return 1

    every = "all" in names
    wanted = list(FIGURES) if every else list(dict.fromkeys(names))
    pooled = pool(loaded)
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"\n{'=' * 78}\nPlots")
    failed = False
    for name in wanted:
        figure = FIGURES.get(name)
        if figure is None:
            print(
                f"ERROR: unknown figure {name!r} (expected: {', '.join(FIGURES)}, all)"
            )
            failed = True
            continue
        why = figure(pooled, out_dir, plt)
        plt.close("all")
        if why is None:
            continue
        if every:
            print(f"  skipping {name}: {why}")
        else:
            print(f"ERROR: {name} cannot be drawn: {why}", file=sys.stderr)
            failed = True
    return 1 if failed else 0
