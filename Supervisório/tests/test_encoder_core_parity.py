import csv
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


TEST_DIR = Path(__file__).resolve().parent
REPO_ROOT = TEST_DIR.parents[1]


def _find_probe():
    override = os.environ.get("ENCODER_STATE_PROBE")
    candidates = [Path(override)] if override else []
    candidates.extend(
        [
            REPO_ROOT / "EncoderCore" / "build_vs2022" / "Release" / "encoder_state_probe.exe",
            REPO_ROOT / "EncoderCore" / "build" / "Release" / "encoder_state_probe.exe",
            REPO_ROOT / "EncoderCore" / "build" / "encoder_state_probe.exe",
        ]
    )
    return next((path for path in candidates if path and path.is_file()), None)


class EncoderCoreParityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.probe = _find_probe()
        with (TEST_DIR / "fixtures" / "encoder_replay_cases.json").open("r", encoding="utf-8") as stream:
            cls.cases = json.load(stream)["cases"]

    def _run_case(self, name, allow_quarantine=False):
        if self.probe is None:
            self.skipTest("encoder_state_probe is not built")
        case = self.cases[name]
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "samples.csv"
            with path.open("w", newline="", encoding="ascii") as stream:
                writer = csv.writer(stream, lineterminator="\n")
                writer.writerow(["t_s", "angle_deg", "quarantine"])
                for item in case["samples"]:
                    writer.writerow(
                        [
                            item[0],
                            "NULL" if item[1] is None else item[1],
                            item[2] if len(item) > 2 else 0,
                        ]
                    )
            command = [
                str(self.probe),
                "--input", str(path),
                "--radius", str(case["radius_mm"]),
                "--max-speed", str(case["max_target_speed_mm_s"]),
            ]
            if allow_quarantine:
                command.append("--allow-quarantine")
            result = subprocess.run(command, capture_output=True, text=True, check=True)
            return json.loads(result.stdout)

    def test_wrap_parity(self):
        for name in ("wrap_forward", "wrap_reverse"):
            result = self._run_case(name)
            self.assertEqual(result["unwrapped"], self.cases[name]["expected_unwrapped_deg"])

    def test_invalid_sample_parity(self):
        missing = self._run_case("missing_samples")
        jump = self._run_case("impossible_jump")
        quarantine = self._run_case("quarantine_gap")
        self.assertEqual(missing["accepted"], self.cases["missing_samples"]["expected_valid_rows"])
        self.assertAlmostEqual(missing["max_gap_s"], self.cases["missing_samples"]["expected_max_valid_gap_s"])
        self.assertEqual(jump["rejected_glitches"], self.cases["impossible_jump"]["expected_rejected_glitches"])
        self.assertEqual(quarantine["quarantine"], self.cases["quarantine_gap"]["expected_quarantine_rows"])
        self.assertEqual(quarantine["accepted"], self.cases["quarantine_gap"]["expected_operational_valid_rows"])

    def test_direction_change_parity(self):
        result = self._run_case("direction_change")
        self.assertEqual(result["reversals"], self.cases["direction_change"]["expected_completed_strokes"])


if __name__ == "__main__":
    unittest.main()
