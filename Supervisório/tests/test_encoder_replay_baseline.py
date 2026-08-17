import json
import math
import unittest
from pathlib import Path


TEST_DIR = Path(__file__).resolve().parent
REPO_ROOT = TEST_DIR.parents[1]

from encoder_replay_baseline import (
    assert_hashes,
    continuous_metrics,
    detect_reciprocating_strokes,
    interpolate_crossing_time,
    load_case_samples,
    load_dataset_samples,
    load_manifest,
    prepare_replay,
    resolve_dataset,
    wrapped_delta_deg,
)


class EncoderReplayContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cases = load_manifest(TEST_DIR / "fixtures" / "encoder_replay_cases.json")["cases"]

    def _prepare_case(self, name, allow_quarantine=False):
        case = self.cases[name]
        return case, prepare_replay(
            load_case_samples(case),
            radius_mm=case["radius_mm"],
            max_target_speed_mm_s=case["max_target_speed_mm_s"],
            allow_quarantine=allow_quarantine,
        )

    def test_wrap_in_both_directions(self):
        for name in ("wrap_forward", "wrap_reverse"):
            case, replay = self._prepare_case(name)
            actual = [sample.unwrapped_deg for sample in replay.samples]
            self.assertEqual(actual, case["expected_unwrapped_deg"])
        self.assertEqual(wrapped_delta_deg(0.0, 359.0), 1.0)
        self.assertEqual(wrapped_delta_deg(359.0, 0.0), -1.0)

    def test_stationary_jitter_is_not_valid_motion(self):
        case, replay = self._prepare_case("jitter_stationary")
        with self.assertRaisesRegex(ValueError, "constant|insufficient"):
            continuous_metrics(replay, case["radius_mm"])

    def test_quarantine_is_excluded_from_operational_path(self):
        case, operational = self._prepare_case("quarantine_gap", allow_quarantine=False)
        _, offline = self._prepare_case("quarantine_gap", allow_quarantine=True)
        self.assertEqual(operational.quarantine_rows, case["expected_quarantine_rows"])
        self.assertEqual(operational.valid_rows, case["expected_operational_valid_rows"])
        self.assertEqual(offline.valid_rows, case["expected_offline_valid_rows"])
        self.assertEqual(offline.rejected_glitches, 1)

    def test_missing_samples_report_the_consecutive_gap(self):
        case, replay = self._prepare_case("missing_samples")
        self.assertEqual(replay.valid_rows, case["expected_valid_rows"])
        self.assertAlmostEqual(replay.max_valid_gap_s, case["expected_max_valid_gap_s"], places=9)

    def test_impossible_jump_is_rejected_without_moving_the_anchor(self):
        case, replay = self._prepare_case("impossible_jump")
        self.assertEqual(replay.rejected_glitches, case["expected_rejected_glitches"])
        self.assertEqual(replay.valid_rows, case["expected_valid_rows"])
        accepted = [sample.unwrapped_deg for sample in replay.samples if sample.unwrapped_deg is not None]
        self.assertEqual(accepted, [10.0, 10.5, 11.5])

    def test_direction_change_delimits_one_completed_stroke(self):
        case, replay = self._prepare_case("direction_change")
        strokes = detect_reciprocating_strokes(
            replay,
            radius_mm=case["radius_mm"],
            course_mm=case["course_mm"],
        )
        completed = [stroke for stroke in strokes if stroke.ended_by_reversal]
        self.assertEqual(len(completed), case["expected_completed_strokes"])
        self.assertEqual(completed[0].direction, case["expected_first_direction"])

    def test_interpolated_boundary_does_not_have_n_minus_one_bias(self):
        # At 20 mm/s, the 4 mm boundaries occur exactly every 0.2 s even
        # when neither boundary is itself a sample timestamp.
        first = interpolate_crossing_time(0.18, 3.6, 0.20, 4.0, 4.0)
        second = interpolate_crossing_time(0.38, 7.6, 0.40, 8.0, 8.0)
        self.assertAlmostEqual(4.0 / (second - first), 20.0, places=9)


class RealEncoderBaselineTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest = load_manifest(TEST_DIR / "fixtures" / "encoder_real_baselines.json")

    def _load_real(self, name):
        dataset = self.manifest["datasets"][name]
        folder = resolve_dataset(REPO_ROOT, dataset)
        if folder is None:
            self.skipTest(
                f"real replay data unavailable for {name}; set DEVRECIPROC_REPLAY_DATA"
            )
        assert_hashes(folder, dataset)
        return dataset, folder, load_dataset_samples(folder, dataset)

    def test_continuous_modbus_loss_golden(self):
        dataset, _, samples = self._load_real("continuous_modbus_loss")
        params = dataset["parameters"]
        expected = dataset["expected"]
        replay = prepare_replay(
            samples,
            radius_mm=params["radius_mm"],
            max_target_speed_mm_s=params["max_target_speed_mm_s"],
            # PosEncExt in this historical _T already contains the offline
            # reconstruction; preserving it is part of this frozen baseline.
            allow_quarantine=True,
        )
        metrics = continuous_metrics(replay, params["radius_mm"])

        self.assertEqual(replay.source_rows, expected["source_rows"])
        self.assertEqual(replay.quarantine_rows, expected["quarantine_rows"])
        self.assertAlmostEqual(replay.sample_rate_hz, expected["sample_rate_hz"], places=6)
        self.assertAlmostEqual(metrics.distance_mm, expected["external_distance_mm"], places=5)
        self.assertAlmostEqual(metrics.turns, expected["external_turns"], places=5)
        self.assertAlmostEqual(metrics.reverse_fraction, expected["reverse_fraction"], delta=0.00001)
        self.assertAlmostEqual(metrics.global_speed_mm_s, expected["global_speed_mm_s"], delta=0.002)
        self.assertLess(abs(metrics.distance_mm - params["programmed_distance_mm"]) / params["programmed_distance_mm"], 0.005)

    def test_reciprocating_multispeed_golden_exposes_offline_false_strokes(self):
        dataset, _, samples = self._load_real("reciprocating_multispeed")
        params = dataset["parameters"]
        expected = dataset["expected"]
        replay = prepare_replay(
            samples,
            radius_mm=params["radius_mm"],
            max_target_speed_mm_s=params["max_target_speed_mm_s"],
            allow_quarantine=True,
        )
        strokes = detect_reciprocating_strokes(
            replay,
            radius_mm=params["radius_mm"],
            course_mm=params["course_mm"],
        )
        expected_count = math.ceil(params["programmed_distance_mm"] / params["course_mm"])
        baseline_strokes = strokes[:expected_count]
        emitted_distance = sum(stroke.length_mm for stroke in baseline_strokes)
        total_distance = sum(stroke.length_mm for stroke in strokes)
        short_emitted = sum(stroke.length_mm < 0.75 * params["course_mm"] for stroke in baseline_strokes)
        short_total = sum(stroke.length_mm < 0.75 * params["course_mm"] for stroke in strokes)

        self.assertEqual(replay.source_rows, expected["source_rows"])
        self.assertAlmostEqual(replay.sample_rate_hz, expected["sample_rate_hz"], places=6)
        self.assertEqual(len(strokes), expected["offline_detected_strokes_total"])
        self.assertEqual(len(baseline_strokes), expected["offline_emitted_strokes"])
        self.assertEqual(short_emitted, expected["offline_short_strokes_emitted_below_75pct"])
        self.assertEqual(short_total, expected["offline_short_strokes_total_below_75pct"])
        self.assertAlmostEqual(emitted_distance, expected["offline_emitted_distance_mm"], places=5)
        self.assertAlmostEqual(total_distance, expected["offline_external_distance_mm"], places=5)
        # This inequality is intentional: it freezes the known disagreement
        # instead of declaring the temporary offline segmentation authoritative.
        self.assertNotEqual(expected["drive_completed_strokes"], expected["offline_emitted_strokes"])


if __name__ == "__main__":
    unittest.main()
