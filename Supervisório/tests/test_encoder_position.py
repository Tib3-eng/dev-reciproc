import csv
import os
import sys
import tempfile
import unittest
from types import SimpleNamespace


SUPERVISOR_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if SUPERVISOR_DIR not in sys.path:
    sys.path.insert(0, SUPERVISOR_DIR)

import orchestrator_runtime as runtime


class EncoderPositionMergeTests(unittest.TestCase):
    def test_appends_position_and_quarantine_flag(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "result.csv")
            with open(path, "w", newline="", encoding="utf-8") as stream:
                writer = csv.writer(stream, delimiter=";", lineterminator="\n")
                writer.writerow(["idx", "t_s", "ch1", "ch2", "ch3", "dlg_err"])
                writer.writerow(["0", "0.0", "1", "2", "361.25", "0"])
                writer.writerow(["1", "0.02", "1", "2", "-0.5", "0"])
                writer.writerow(["2", "0.04", "NULL", "NULL", "NULL", "1"])

            state = SimpleNamespace(merge_csv=path)
            runtime._append_external_encoder_position(state)

            with open(path, "r", newline="", encoding="utf-8") as stream:
                rows = list(csv.reader(stream, delimiter=";"))

            self.assertEqual(rows[0][-2:], ["PosEncExt", "PosEncExt_Quarentena"])
            self.assertEqual(rows[1][-2:], ["1.25", "0"])
            self.assertEqual(rows[2][-2:], ["359.5", "0"])
            self.assertEqual(rows[3][-2:], ["NULL", "0"])
            self.assertEqual(state.encoder_quarantine_samples, 0)
            self.assertEqual(state.encoder_quarantine_fraction, 0.0)

    def test_quarantines_and_reconstructs_unstable_wrap_transition(self):
        raw = [
            348.0, 349.0, 350.0, 351.0,
            9.0, 341.9, 28.7, 256.5, 349.4, 2.5, 4.5, 357.0, 7.2,
            8.0, 9.0, 10.0, 11.0, 12.0,
        ]
        filtered, quarantine = runtime._filter_external_encoder_angles(raw, 1.0)
        unwrapped = [filtered[0]]
        for angle in filtered[1:]:
            previous = unwrapped[-1]
            unwrapped.append(runtime._nearest_unwrapped_angle(angle, previous))

        self.assertEqual(quarantine[:4], [0, 0, 0, 0])
        self.assertTrue(all(flag == 1 for flag in quarantine[4:-1]))
        self.assertEqual(quarantine[-1], 0)
        self.assertLess(max(abs(b - a) for a, b in zip(unwrapped, unwrapped[1:])), 4.0)
        self.assertAlmostEqual(unwrapped[-1], 372.0, places=6)

    def test_does_not_duplicate_existing_column(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "result.csv")
            with open(path, "w", newline="", encoding="utf-8") as stream:
                stream.write("idx;ch3;PosEncExt;PosEncExt_Quarentena\n0;10;10;0\n")

            runtime._append_external_encoder_position(
                SimpleNamespace(merge_csv=path)
            )

            with open(path, "r", encoding="utf-8") as stream:
                self.assertEqual(
                    stream.readline().strip(),
                    "idx;ch3;PosEncExt;PosEncExt_Quarentena",
                )


if __name__ == "__main__":
    unittest.main()
