"""
points.py — sweep CSVs read back as points, shared by every analysis.

A CSV belongs to the `Bench` whose name its filename starts with (sweep.py writes
results/<bench.name>_<factors>_<timestamp>.csv). Each row is one run, runs with
the same axis values and factor values are one point.
"""

import csv
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING

from tracecliff.benches import ALL_BENCHES
from tracecliff.exception import SweepCsvError
from tracecliff.sweep import CST_FIELDS, FACTOR_FLAGS, Bench

if TYPE_CHECKING:
    from tracecliff.analyzer import Loss

# The cs-trace columns averaged over a point's runs.
METRICS = [f for f in CST_FIELDS if f != "binary_name"]


@dataclass(frozen=True, order=True)
class PointKey:
    """One swept point: its bench axis values and its factor values.

    axes  the Bench's axis values, outermost first, iters last
    arms  (factor, value) for every factor column, in CSV order
    """

    axes: tuple[int, ...]
    arms: tuple[tuple[str, str], ...]

    @property
    def iters(self) -> int:
        return self.axes[-1]

    @property
    def workload(self) -> tuple[tuple[int, ...], str | None]:
        """The benchmark and address filter, which decide what a lossless trace
        contains; None when the CSV has no addrfilter column."""
        return self.axes, self.arm("addrfilter")

    def arm(self, name: str) -> str | None:
        return dict(self.arms).get(name)


@dataclass
class Point:
    """The runs at one PointKey."""

    key: PointKey
    binary_name: str = ""
    n_runs: int = 0
    samples: dict[str, list[float]] = field(default_factory=lambda: defaultdict(list))
    # Filled in by the analyzer.
    loss: "Loss | None" = None

    def add(self, row: dict) -> None:
        self.binary_name = row.get("binary_name") or self.binary_name
        self.n_runs += 1
        for m in METRICS:
            v = to_float(row.get(m))
            if v is not None:
                self.samples[m].append(v)

    def mean(self, metric: str) -> float | None:
        vals = self.samples.get(metric)
        return sum(vals) / len(vals) if vals else None

    def n_ok(self, metric: str) -> int:
        """Runs that recorded `metric`."""
        return len(self.samples.get(metric, ()))


def to_float(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def bench_for(path: Path) -> Bench | None:
    """The Bench that wrote this CSV, from the longest matching name prefix."""
    matches = [b for b in ALL_BENCHES if path.name.startswith(f"{b.name}_")]
    return max(matches, key=lambda b: len(b.name), default=None)


def load_points(path: Path, bench: Bench) -> dict[PointKey, Point]:
    """Every point in a CSV written for `bench`, runs grouped."""
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        fields = reader.fieldnames or []
        axis_fields = [a.field for a in bench.axes]
        missing = [a for a in axis_fields if a not in fields]
        if missing:
            raise SweepCsvError(
                f"{path.name}: no {', '.join(missing)} column for {bench.name}"
            )
        arm_fields = [f for f in fields if f in FACTOR_FLAGS]

        points: dict[PointKey, Point] = {}
        for row in reader:
            try:
                axes = tuple(int(row[a]) for a in axis_fields)
            except ValueError:
                raise SweepCsvError(
                    f"{path.name}: non-integer axis value on line {reader.line_num}"
                ) from None
            key = PointKey(axes, tuple((f, row[f]) for f in arm_fields))
            if key not in points:
                points[key] = Point(key)
            points[key].add(row)
    return points


def arm_values(points) -> dict[str, list[str]]:
    """Each factor's values, in the order the sweep walked them."""
    out: dict[str, list[str]] = {}
    for key in points:
        for name, value in key.arms:
            values = out.setdefault(name, [])
            if value not in values:
                values.append(value)
    return out


def axis_values(points, bench: Bench) -> dict[str, list[int]]:
    """Each axis's values, sorted."""
    return {
        a.field: sorted({k.axes[i] for k in points}) for i, a in enumerate(bench.axes)
    }
