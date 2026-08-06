import csv
import math
import os
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace


SUPERVISOR_DIR = Path(__file__).resolve().parents[1]
if str(SUPERVISOR_DIR) not in sys.path:
    sys.path.insert(0, str(SUPERVISOR_DIR))

import orchestrator_runtime as orch


class ReciprocatingProcessingTests(unittest.TestCase):
    def test_final_merge_is_cropped_at_external_encoder_endpoint(self):
        with tempfile.TemporaryDirectory() as tmp:
            merged = os.path.join(tmp, "result_T.csv")
            with open(merged, "w", newline="", encoding="utf-8") as f:
                w = csv.writer(f, delimiter=";", lineterminator="\n")
                w.writerow(["idx", "PosEncExt"])
                for idx in range(8):
                    w.writerow([idx, 10.0 + idx])
            state = SimpleNamespace(
                reciprocating=True,
                merge_csv=merged,
                reciprocating_final_encoder_idx=5,
            )
            orch._crop_reciprocating_merge_at_encoder_endpoint(state)
            with open(merged, "r", newline="", encoding="utf-8") as f:
                rows = list(csv.DictReader(f, delimiter=";"))
            self.assertEqual([int(row["idx"]) for row in rows], list(range(6)))

    def test_m_uses_one_target_speed_per_completed_stroke(self):
        with tempfile.TemporaryDirectory() as tmp:
            dlg = os.path.join(tmp, "dlg.csv")
            drive = os.path.join(tmp, "drive.csv")
            dp = os.path.join(tmp, "result_DP.csv")
            motion = os.path.join(tmp, "result_M.csv")

            with open(dlg, "w", newline="", encoding="ascii") as f:
                w = csv.writer(f, lineterminator="\n")
                w.writerow(["idx", "t_qpc", "t_s", "ch1", "ch2", "ch3", "ch4", "ch5", "ch6", "ch7", "ch8", "atrito", "err"])
                for idx in range(8):
                    force = -1.0 if idx < 4 else 1.0
                    w.writerow([idx, idx, idx * 0.1, force, 0, 0, 0, 0, 0, 0, 0, force, 0])

            positions = [0, 100, 200, 300, 300, 200, 100, 0]
            with open(drive, "w", newline="", encoding="ascii") as f:
                w = csv.writer(f, lineterminator="\n")
                w.writerow(["idx", "t_qpc", "t_s", "pos", "rpm", "pos_err", "rpm_err", "pos_mod"])
                for idx, pos in enumerate(positions):
                    rpm = 10 if idx < 4 else -20
                    w.writerow([idx, idx, idx * 0.1, pos, rpm, 0, 0, 1000])

            with open(os.path.join(tmp, "a5_speed_events.log"), "w", encoding="ascii") as f:
                f.write("RECIP_INIT pos=0 pos_mod=1000\n")
                f.write("RECIP_REVERSE idx=3 error_counts=0 completed_segment=0\n")
                f.write("RECIP_DONE idx=7 error_counts=0 completed_segment=1\n")

            state = SimpleNamespace(
                dlg_csv=dlg,
                drive_csv=drive,
                turn_dist_csv=dp,
                turn_vp_csv=motion,
                dynamic_offset_n=0.0,
                force_normal_n=1.0,
                reciprocating_course_mm=2.0 * math.pi * 10.0 * 0.3,
                reciprocating_edge_filter_pct=0.0,
                target_speed_schedule=[(5.0, 10.0), (8.0, 10.0)],
            )
            orch._rebuild_reciprocating_csv_from_logs(state, 1.0, 10.0, 5.0)

            with open(motion, "r", newline="", encoding="utf-8") as f:
                rows = list(csv.DictReader(f, delimiter=";"))
            self.assertEqual(len(rows), 2)
            self.assertEqual(rows[0]["VELOCIDADE_ALVO"], "5.000000")
            self.assertEqual(rows[1]["VELOCIDADE_ALVO"], "8.000000")
            self.assertIn("VELOCIDADE_MEDIA", rows[0])
            self.assertNotIn("VELOCIDADE", rows[0])


if __name__ == "__main__":
    unittest.main()
