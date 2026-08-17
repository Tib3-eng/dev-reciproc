"""Reference-only replay helpers for the external encoder migration baseline.

This module deliberately lives under ``tests``.  It reproduces the relevant
offline rules used by the temporary ReprocessaEncoder utility so that later
implementations in C can be compared with a frozen baseline.  It is not used by
the supervisor or by the real-time controller.
"""

from __future__ import annotations

import csv
import hashlib
import json
import math
import os
from dataclasses import dataclass
from pathlib import Path
from statistics import median
from typing import Iterable, List, Optional, Sequence


@dataclass
class ReplaySample:
    idx: int
    t_s: float
    angle_deg: Optional[float]
    quarantine: bool = False
    unwrapped_deg: Optional[float] = None


@dataclass
class PreparedReplay:
    samples: List[ReplaySample]
    source_rows: int
    valid_rows: int
    rejected_glitches: int
    quarantine_rows: int
    sample_rate_hz: float
    max_valid_gap_s: float


@dataclass
class ContinuousMetrics:
    direction: int
    forward_deg: float
    reverse_deg: float
    net_deg: float
    distance_mm: float
    turns: float
    reverse_fraction: float
    duration_s: float
    global_speed_mm_s: float


@dataclass
class StrokeMetric:
    start_index: int
    end_index: int
    direction: int
    length_mm: float
    ended_by_reversal: bool


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def load_manifest(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def wrapped_delta_deg(current: float, previous: float) -> float:
    delta = current - previous
    while delta > 180.0:
        delta -= 360.0
    while delta < -180.0:
        delta += 360.0
    return delta


def interpolate_crossing_time(
    t0: float,
    x0: float,
    t1: float,
    x1: float,
    boundary: float,
) -> float:
    """Return the linear crossing time for a monotonic boundary crossing."""
    if not t1 > t0 or x1 == x0:
        raise ValueError("crossing requires increasing time and movement")
    fraction = (boundary - x0) / (x1 - x0)
    if fraction < -1e-12 or fraction > 1.0 + 1e-12:
        raise ValueError("boundary is outside the sample pair")
    return t0 + min(1.0, max(0.0, fraction)) * (t1 - t0)


def _as_float(value: object) -> Optional[float]:
    if value is None:
        return None
    text = str(value).strip()
    if not text or text.upper() == "NULL":
        return None
    try:
        number = float(text)
    except ValueError:
        return None
    return number if math.isfinite(number) else None


def read_test_samples(path: Path) -> List[ReplaySample]:
    samples: List[ReplaySample] = []
    with path.open("r", newline="", encoding="utf-8-sig") as stream:
        first = stream.readline()
        delimiter = ";" if first.count(";") >= first.count(",") else ","
        stream.seek(0)
        reader = csv.DictReader(stream, delimiter=delimiter)
        if not reader.fieldnames:
            raise ValueError(f"CSV without header: {path}")
        fields = {name.lower(): name for name in reader.fieldnames}
        time_key = fields.get("t_s")
        angle_key = fields.get("posencext") or fields.get("ch3")
        quarantine_key = fields.get("posencext_quarentena")
        idx_key = fields.get("idx")
        dlg_error_key = fields.get("dlg_err")
        if not time_key or not angle_key:
            raise ValueError(f"CSV without t_s/PosEncExt: {path}")

        for row_number, row in enumerate(reader):
            t_s = _as_float(row.get(time_key))
            if t_s is None:
                continue
            dlg_ok = str(row.get(dlg_error_key, "0")).strip() in ("", "0")
            angle = _as_float(row.get(angle_key)) if dlg_ok else None
            idx_value = _as_float(row.get(idx_key)) if idx_key else None
            quarantine_value = _as_float(row.get(quarantine_key)) if quarantine_key else None
            samples.append(
                ReplaySample(
                    idx=int(idx_value) if idx_value is not None else row_number,
                    t_s=t_s,
                    angle_deg=angle,
                    quarantine=bool(quarantine_value),
                )
            )
    return samples

def prepare_replay(
    source: Sequence[ReplaySample],
    *,
    radius_mm: float,
    max_target_speed_mm_s: float,
    allow_quarantine: bool,
) -> PreparedReplay:
    """Reproduce the temporary offline utility's unwrap and innovation gate."""
    if radius_mm <= 0.0:
        raise ValueError("radius_mm must be positive")

    samples = [ReplaySample(**vars(item)) for item in source]
    valid_times = [item.t_s for item in samples]
    dts = [b - a for a, b in zip(valid_times, valid_times[1:]) if 0.0 < b - a < 10.0]
    median_dt = median(dts) if dts else 0.02
    expected_rate_deg_s = (
        max_target_speed_mm_s * 180.0 / (math.pi * radius_mm)
        if max_target_speed_mm_s > 0.0
        else 0.0
    )

    last_normalized: Optional[float] = None
    last_unwrapped: Optional[float] = None
    last_time: Optional[float] = None
    rejected = 0
    quarantine_rows = sum(item.quarantine for item in samples)
    accepted_times: List[float] = []

    for sample in samples:
        if sample.angle_deg is None or (sample.quarantine and not allow_quarantine):
            continue
        angle = sample.angle_deg % 360.0
        if last_normalized is None:
            sample.unwrapped_deg = angle
        else:
            delta = wrapped_delta_deg(angle, last_normalized)
            dt = sample.t_s - last_time if last_time is not None and sample.t_s > last_time else median_dt
            gate = expected_rate_deg_s * dt * 5.0 + 2.0 if expected_rate_deg_s > 0.0 else 45.0
            gate = min(179.5, max(3.0, gate))
            if abs(delta) > gate:
                rejected += 1
                continue
            sample.unwrapped_deg = last_unwrapped + delta  # type: ignore[operator]

        last_normalized = angle
        last_unwrapped = sample.unwrapped_deg
        last_time = sample.t_s
        accepted_times.append(sample.t_s)

    gaps = [b - a for a, b in zip(accepted_times, accepted_times[1:])]
    return PreparedReplay(
        samples=samples,
        source_rows=len(samples),
        valid_rows=len(accepted_times),
        rejected_glitches=rejected,
        quarantine_rows=quarantine_rows,
        sample_rate_hz=1.0 / median_dt if median_dt > 0.0 else 0.0,
        max_valid_gap_s=max(gaps, default=0.0),
    )


def continuous_metrics(prepared: PreparedReplay, radius_mm: float, deadband_deg: float = 0.005) -> ContinuousMetrics:
    valid = [item for item in prepared.samples if item.unwrapped_deg is not None]
    if len(valid) < 2:
        raise ValueError("constant or insufficient encoder movement")

    signed_sum = 0.0
    for previous, current in zip(valid, valid[1:]):
        delta = current.unwrapped_deg - previous.unwrapped_deg  # type: ignore[operator]
        if abs(delta) >= deadband_deg:
            signed_sum += delta
    direction = 1 if signed_sum >= 0.0 else -1

    forward = 0.0
    reverse = 0.0
    net = 0.0
    for previous, current in zip(valid, valid[1:]):
        delta = direction * (current.unwrapped_deg - previous.unwrapped_deg)  # type: ignore[operator]
        if delta >= deadband_deg:
            forward += delta
            net += delta
        elif delta <= -deadband_deg:
            reverse -= delta
            net = max(0.0, net + delta)

    if net <= 0.0:
        raise ValueError("constant or insufficient encoder movement")
    circumference = 2.0 * math.pi * radius_mm
    distance = net * circumference / 360.0
    duration = valid[-1].t_s - valid[0].t_s
    return ContinuousMetrics(
        direction=direction,
        forward_deg=forward,
        reverse_deg=reverse,
        net_deg=net,
        distance_mm=distance,
        turns=net / 360.0,
        reverse_fraction=reverse / forward if forward > 0.0 else 0.0,
        duration_s=duration,
        global_speed_mm_s=distance / duration if duration > 0.0 else math.nan,
    )


def detect_reciprocating_strokes(
    prepared: PreparedReplay,
    *,
    radius_mm: float,
    course_mm: float,
) -> List[StrokeMetric]:
    """Reproduce the offline reversal segmentation, including its limitations."""
    if course_mm <= 0.0:
        raise ValueError("course_mm must be positive")
    samples = prepared.samples
    mm_per_degree = 2.0 * math.pi * radius_mm / 360.0
    deadband_mm = max(0.001, course_mm * 0.0002)
    confirmation_mm = max(0.02, course_mm * 0.005)

    previous_valid: Optional[int] = None
    motion_start: Optional[int] = None
    candidate_start: Optional[int] = None
    last_directional: Optional[int] = None
    current_direction = 0
    first_direction = 0
    candidate_direction = 0
    candidate_count = 0
    candidate_distance = 0.0
    boundaries: List[int] = []

    for index, sample in enumerate(samples):
        if sample.unwrapped_deg is None:
            continue
        if previous_valid is None:
            previous_valid = index
            continue
        previous = samples[previous_valid]
        movement_mm = (sample.unwrapped_deg - previous.unwrapped_deg) * mm_per_degree  # type: ignore[operator]
        if abs(movement_mm) < deadband_mm:
            previous_valid = index
            continue
        sign = 1 if movement_mm > 0.0 else -1

        if current_direction == 0:
            if candidate_direction != sign:
                candidate_direction = sign
                candidate_count = 1
                candidate_distance = abs(movement_mm)
                candidate_start = previous_valid
            else:
                candidate_count += 1
                candidate_distance += abs(movement_mm)
            if candidate_count >= 2 and candidate_distance >= confirmation_mm:
                current_direction = candidate_direction
                first_direction = current_direction
                motion_start = candidate_start
                last_directional = index
                candidate_direction = 0
                candidate_count = 0
                candidate_distance = 0.0
        elif sign == current_direction:
            candidate_direction = 0
            candidate_count = 0
            candidate_distance = 0.0
            candidate_start = None
            last_directional = index
        else:
            if candidate_direction != sign:
                candidate_direction = sign
                candidate_count = 1
                candidate_distance = abs(movement_mm)
                candidate_start = previous_valid
            else:
                candidate_count += 1
                candidate_distance += abs(movement_mm)
            if candidate_count >= 2 and candidate_distance >= confirmation_mm:
                if candidate_start is not None:
                    boundaries.append(candidate_start)
                current_direction = candidate_direction
                last_directional = index
                candidate_direction = 0
                candidate_count = 0
                candidate_distance = 0.0
                candidate_start = None
        previous_valid = index

    if motion_start is None or last_directional is None or current_direction == 0:
        raise ValueError("reciprocating movement not detected")

    strokes: List[StrokeMetric] = []
    start = motion_start
    direction = first_direction
    for end in boundaries:
        if end <= start:
            continue
        start_angle = samples[start].unwrapped_deg
        max_progress = 0.0
        for sample in samples[start : end + 1]:
            if sample.unwrapped_deg is None:
                continue
            progress = direction * (sample.unwrapped_deg - start_angle) * mm_per_degree  # type: ignore[operator]
            max_progress = max(max_progress, progress)
        strokes.append(StrokeMetric(start, end, direction, max_progress, True))
        start = end
        direction = -direction

    if last_directional > start:
        start_angle = samples[start].unwrapped_deg
        max_progress = 0.0
        for sample in samples[start : last_directional + 1]:
            if sample.unwrapped_deg is None:
                continue
            progress = direction * (sample.unwrapped_deg - start_angle) * mm_per_degree  # type: ignore[operator]
            max_progress = max(max_progress, progress)
        strokes.append(StrokeMetric(start, last_directional, direction, max_progress, False))
    return strokes


def resolve_dataset(repo_root: Path, dataset: dict) -> Optional[Path]:
    override = os.environ.get("DEVRECIPROC_REPLAY_DATA")
    if override:
        candidate = Path(override) / dataset["override_relative_path"]
        if candidate.is_dir():
            return candidate

    for pattern in dataset.get("repo_globs", []):
        matches = sorted(repo_root.glob(pattern))
        if matches:
            return matches[0]

    desktop = Path.home() / "Desktop" / "Repositorio"
    for relative in dataset.get("desktop_repository_paths", []):
        candidate = desktop / relative
        if candidate.is_dir():
            return candidate
    return None


def load_dataset_samples(folder: Path, dataset: dict) -> List[ReplaySample]:
    return read_test_samples(folder / dataset["test_file"])


def assert_hashes(folder: Path, dataset: dict) -> None:
    for name, expected in dataset["sha256"].items():
        actual = file_sha256(folder / name)
        if actual != expected:
            raise AssertionError(f"SHA-256 mismatch for {name}: {actual} != {expected}")


def load_case_samples(case: dict) -> List[ReplaySample]:
    samples = []
    for index, item in enumerate(case["samples"]):
        samples.append(
            ReplaySample(
                idx=index,
                t_s=float(item[0]),
                angle_deg=None if item[1] is None else float(item[1]),
                quarantine=bool(item[2]) if len(item) > 2 else False,
            )
        )
    return samples
