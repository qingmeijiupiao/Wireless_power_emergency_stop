#!/usr/bin/env python3
"""从放电 CSV 生成固件使用的电压-SOC 插值表。"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Sample:
    remaining_percent: float
    voltage_mv: int


def load_samples(path: Path) -> list[Sample]:
    with path.open("r", encoding="utf-8", newline="") as source:
        rows = [
            Sample(
                remaining_percent=float(row["remaining_percent"]),
                voltage_mv=int(row["voltage_mv"]),
            )
            for row in csv.DictReader(source)
        ]
    if len(rows) < 2:
        raise ValueError("曲线至少需要两个采样点")
    return sorted(rows, key=lambda sample: sample.remaining_percent)


def interpolate_voltage(samples: list[Sample], percent: int) -> float:
    target = float(percent)
    if target <= samples[0].remaining_percent:
        return float(samples[0].voltage_mv)
    if target >= samples[-1].remaining_percent:
        return float(samples[-1].voltage_mv)

    left = 0
    right = len(samples) - 1
    while left + 1 < right:
        middle = (left + right) // 2
        if samples[middle].remaining_percent < target:
            left = middle
        else:
            right = middle

    lower = samples[left]
    upper = samples[right]
    span = upper.remaining_percent - lower.remaining_percent
    ratio = (target - lower.remaining_percent) / span
    return lower.voltage_mv + (upper.voltage_mv - lower.voltage_mv) * ratio


def build_points(samples: list[Sample], full_voltage_mv: int) -> list[tuple[int, int]]:
    measured_full_mv = samples[-1].voltage_mv
    points: list[tuple[int, int]] = []
    previous_mv: int | None = None

    for percent in range(101):
        measured_mv = interpolate_voltage(samples, percent)
        calibrated_mv = round(measured_mv * full_voltage_mv / measured_full_mv)
        if previous_mv is not None:
            # 插值模块要求输入严格单调；局部 ADC 回升和量化平台在此收敛。
            calibrated_mv = max(calibrated_mv, previous_mv + 1)
        points.append((calibrated_mv, percent))
        previous_mv = calibrated_mv

    points[-1] = (full_voltage_mv, 100)
    if points[-2][0] >= full_voltage_mv:
        raise ValueError("曲线高电量端无法在满电电压以内保持严格单调")
    return points


def write_header(
    path: Path,
    points: list[tuple[int, int]],
    source_name: str,
    measured_full_mv: int,
    full_voltage_mv: int,
) -> None:
    entries = "\n".join(
        f"    Point{{{voltage_mv}, {percent}}},"
        for voltage_mv, percent in points
    )
    content = f"""// 由 scripts/generate_battery_curve_header.py 自动生成，请勿手工修改。
// 数据源: {source_name}
// 满电基准: 测量 {measured_full_mv} mV -> 实际 {full_voltage_mv} mV
#ifndef BATTERY_CURVE_GENERATED_H
#define BATTERY_CURVE_GENERATED_H

#include <array>
#include <cstdint>

namespace BatteryCurveGenerated {{

struct Point {{
    int voltage_mv;
    uint8_t percent;
}};

inline constexpr std::array<Point, {len(points)}> POINTS = {{{{
{entries}
}}}};

}} // namespace BatteryCurveGenerated

#endif
"""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8", newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="process_battery_curve.py 生成的 CSV")
    parser.add_argument("output", type=Path, help="输出的 C++ 头文件")
    parser.add_argument("--full-voltage-mv", type=int, default=4200)
    args = parser.parse_args()

    samples = load_samples(args.input)
    points = build_points(samples, args.full_voltage_mv)
    write_header(
        args.output,
        points,
        args.input.name,
        samples[-1].voltage_mv,
        args.full_voltage_mv,
    )
    print(
        f"points={len(points)} voltage_mv={points[0][0]}..{points[-1][0]} "
        f"output={args.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
