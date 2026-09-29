#!/usr/bin/env python3
"""Convert a blackbox dump into battery discharge curve data."""

from __future__ import annotations

import argparse
import csv
import html
import re
import sys
from dataclasses import dataclass
from pathlib import Path


CURVE_RE = re.compile(
    r"\bcurve:\s+n=(?P<sequence>\d+)\s+ms=(?P<elapsed_ms>\d+)\s+mv=(?P<voltage_mv>\d+)\b"
)


@dataclass(frozen=True)
class Sample:
    sequence: int
    elapsed_ms: int
    voltage_mv: int


def parse_runs(text: str) -> list[list[Sample]]:
    """Return runs in newest-first order from a newest-first blackbox dump."""
    runs: list[list[Sample]] = []
    current: list[Sample] = []
    previous_sequence: int | None = None

    for line in text.splitlines():
        match = CURVE_RE.search(line)
        if match is None:
            continue

        sample = Sample(
            sequence=int(match.group("sequence")),
            elapsed_ms=int(match.group("elapsed_ms")),
            voltage_mv=int(match.group("voltage_mv")),
        )

        # Dump output is newest-first, so sequence numbers normally decrease.
        # A non-decreasing number marks the previous test run.
        if previous_sequence is not None and sample.sequence >= previous_sequence:
            if current:
                runs.append(current)
            current = []

        current.append(sample)
        previous_sequence = sample.sequence

    if current:
        runs.append(current)

    return runs


def normalize_run(samples_newest_first: list[Sample]) -> list[Sample]:
    unique: dict[int, Sample] = {}
    for sample in samples_newest_first:
        unique.setdefault(sample.sequence, sample)
    return sorted(unique.values(), key=lambda sample: sample.sequence)


def write_csv(path: Path, samples: list[Sample]) -> None:
    final_elapsed_ms = max(sample.elapsed_ms for sample in samples)
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(
            [
                "sequence",
                "elapsed_ms",
                "elapsed_hours",
                "voltage_mv",
                "voltage_v",
                "discharged_percent",
                "remaining_percent",
            ]
        )
        for sample in samples:
            discharged = (
                sample.elapsed_ms * 100.0 / final_elapsed_ms
                if final_elapsed_ms > 0
                else 0.0
            )
            writer.writerow(
                [
                    sample.sequence,
                    sample.elapsed_ms,
                    f"{sample.elapsed_ms / 3_600_000:.6f}",
                    sample.voltage_mv,
                    f"{sample.voltage_mv / 1000:.4f}",
                    f"{discharged:.4f}",
                    f"{100.0 - discharged:.4f}",
                ]
            )


def svg_points(
    samples: list[Sample],
    x_getter,
    y_getter,
    left: float,
    top: float,
    width: float,
    height: float,
) -> str:
    x_values = [x_getter(sample) for sample in samples]
    y_values = [y_getter(sample) for sample in samples]
    x_min, x_max = min(x_values), max(x_values)
    y_min, y_max = min(y_values), max(y_values)
    x_span = max(x_max - x_min, 1.0)
    y_span = max(y_max - y_min, 1.0)

    points = []
    for x_value, y_value in zip(x_values, y_values):
        x = left + (x_value - x_min) * width / x_span
        y = top + height - (y_value - y_min) * height / y_span
        points.append(f"{x:.2f},{y:.2f}")
    return " ".join(points)


def write_svg(path: Path, samples: list[Sample], source_name: str) -> None:
    width, height = 1000, 620
    left, top, plot_width, plot_height = 90, 55, 850, 480
    final_elapsed_ms = max(sample.elapsed_ms for sample in samples)
    min_mv = min(sample.voltage_mv for sample in samples)
    max_mv = max(sample.voltage_mv for sample in samples)

    points = svg_points(
        samples,
        lambda sample: sample.elapsed_ms,
        lambda sample: sample.voltage_mv,
        left,
        top,
        plot_width,
        plot_height,
    )
    title = html.escape(f"Battery discharge curve - {source_name}")
    duration_hours = final_elapsed_ms / 3_600_000

    svg = f"""<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}">
<rect width="100%" height="100%" fill="white"/>
<text x="{width / 2}" y="30" text-anchor="middle" font-family="sans-serif" font-size="20">{title}</text>
<line x1="{left}" y1="{top}" x2="{left}" y2="{top + plot_height}" stroke="black"/>
<line x1="{left}" y1="{top + plot_height}" x2="{left + plot_width}" y2="{top + plot_height}" stroke="black"/>
<polyline points="{points}" fill="none" stroke="#1565c0" stroke-width="2"/>
<text x="{left + plot_width / 2}" y="{height - 25}" text-anchor="middle" font-family="sans-serif">Elapsed time (hours), 0 to {duration_hours:.3f}</text>
<text x="24" y="{top + plot_height / 2}" text-anchor="middle" font-family="sans-serif" transform="rotate(-90 24 {top + plot_height / 2})">Battery voltage (mV), {min_mv} to {max_mv}</text>
<text x="{left}" y="{top + plot_height + 20}" font-family="sans-serif" font-size="12">0 h</text>
<text x="{left + plot_width}" y="{top + plot_height + 20}" text-anchor="end" font-family="sans-serif" font-size="12">{duration_hours:.3f} h</text>
<text x="{left - 8}" y="{top + 5}" text-anchor="end" font-family="sans-serif" font-size="12">{max_mv}</text>
<text x="{left - 8}" y="{top + plot_height}" text-anchor="end" font-family="sans-serif" font-size="12">{min_mv}</text>
</svg>
"""
    path.write_text(svg, encoding="utf-8")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Process `blackbox dump all` output into CSV and SVG."
    )
    parser.add_argument("input", type=Path, help="Saved Shell/blackbox dump text")
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="CSV output path (default: <input>_curve.csv)",
    )
    parser.add_argument(
        "--svg",
        type=Path,
        help="SVG output path (default: <input>_curve.svg)",
    )
    parser.add_argument(
        "--run",
        type=int,
        default=0,
        help="Run index in newest-first order (default: 0)",
    )
    parser.add_argument(
        "--no-svg",
        action="store_true",
        help="Do not generate an SVG graph",
    )
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.run < 0:
        print("error: --run must be zero or greater", file=sys.stderr)
        return 2

    try:
        text = args.input.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        print(f"error: cannot read {args.input}: {error}", file=sys.stderr)
        return 2

    runs = parse_runs(text)
    if not runs:
        print("error: no `curve: n=... ms=... mv=...` samples found", file=sys.stderr)
        return 1
    if args.run >= len(runs):
        print(
            f"error: run {args.run} not found; available runs: 0..{len(runs) - 1}",
            file=sys.stderr,
        )
        return 1

    samples = normalize_run(runs[args.run])
    output = args.output or args.input.with_name(f"{args.input.stem}_curve.csv")
    svg_output = args.svg or args.input.with_name(f"{args.input.stem}_curve.svg")

    try:
        write_csv(output, samples)
        if not args.no_svg:
            write_svg(svg_output, samples, args.input.name)
    except OSError as error:
        print(f"error: cannot write output: {error}", file=sys.stderr)
        return 2

    first = samples[0]
    last = samples[-1]
    missing = max(0, last.sequence - first.sequence + 1 - len(samples))
    print(f"runs_found={len(runs)} selected_run={args.run}")
    print(
        f"samples={len(samples)} sequence={first.sequence}..{last.sequence} "
        f"missing={missing}"
    )
    print(
        f"duration_ms={last.elapsed_ms} duration_hours={last.elapsed_ms / 3_600_000:.3f} "
        f"voltage_mv={first.voltage_mv}..{last.voltage_mv} "
        f"range_mv={min(s.voltage_mv for s in samples)}..{max(s.voltage_mv for s in samples)}"
    )
    print(f"csv={output}")
    if not args.no_svg:
        print(f"svg={svg_output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
