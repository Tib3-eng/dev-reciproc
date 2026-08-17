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
    def test_info_csv_writer_uses_semicolon_and_preserves_value_punctuation(self):
        import io
        stream = io.StringIO(newline="")
        writer = orch.InfoCsvWriter(stream)
        writer.write("campo,valor,valor2\n")
        writer.write("Evento,texto com, virgula e; ponto,\n")
        stream.seek(0)
        rows = list(csv.reader(stream, delimiter=";"))
        self.assertEqual(rows[0], ["campo", "valor", "valor2"])
        self.assertEqual(rows[1], ["Evento", "texto com, virgula e; ponto", ""])
        self.assertNotIn("campo,valor", stream.getvalue())

    def test_stationary_endpoint_requires_motion_then_stability(self):
        samples = []
        for idx in range(141):
            t_s = idx / 200.0
            if t_s <= 0.35:
                angle = 10.0 - 3.0 * (t_s / 0.35)
            else:
                angle = 7.0 + (0.04 if idx % 2 else -0.04)
            samples.append((idx, t_s, angle))
        endpoint = orch._detect_reciprocating_stationary_endpoint(samples, 10.0)
        self.assertIsNotNone(endpoint)
        self.assertGreaterEqual(endpoint[1], 0.30)
        self.assertLessEqual(endpoint[1], 0.50)

        still_moving = [(idx, idx / 200.0, 10.0 - idx * 0.01) for idx in range(141)]
        self.assertIsNone(
            orch._detect_reciprocating_stationary_endpoint(still_moving, 10.0)
        )

    def test_stationary_endpoint_replay_recovers_last_stroke(self):
        dlg_path = Path(
            r"C:\Users\nicol\Desktop\Repositorio\2026-08-17 - PoD - "
            r"TesteReciprocating_7-1\DadosDev\dlg.csv"
        )
        if not dlg_path.is_file():
            self.skipTest("replay reciprocante de 2026-08-17 indisponivel")
        samples = []
        previous_angle = None
        unwrapped = None
        with dlg_path.open(newline="", encoding="ascii") as stream:
            for row in csv.DictReader(stream):
                t_s = float(row["t_s"])
                if t_s < 179.17 or row["ch3"] == "NULL":
                    continue
                angle = float(row["ch3"])
                if previous_angle is None:
                    unwrapped = angle
                else:
                    delta = angle - previous_angle
                    if delta > 180.0:
                        delta -= 360.0
                    elif delta < -180.0:
                        delta += 360.0
                    unwrapped += delta
                previous_angle = angle
                samples.append((int(row["idx"]), t_s, unwrapped))
        endpoint = orch._detect_reciprocating_stationary_endpoint(samples, 10.0)
        self.assertIsNotNone(endpoint)
        self.assertGreaterEqual(endpoint[1], 179.40)
        self.assertLessEqual(endpoint[1], 179.80)

    def test_startup_encoder_origin_failure_is_reported_as_primary_cause(self):
        with tempfile.TemporaryDirectory() as tmp:
            event_path = os.path.join(tmp, "a5_speed_events.log")
            with open(event_path, "w", encoding="ascii") as stream:
                stream.write(
                    "[15:10:14.102] FATAL RECIP_ENCODER_NO_ORIGIN "
                    "timeout_s=3.000 before_motion=1 motor_started=0\n"
                )
            reason = orch._reciprocating_startup_failure_reason(event_path)
            self.assertEqual(
                reason,
                "Encoder externo não entregou origem válida em 3 s; motor não foi ligado",
            )

    def test_startup_failure_parser_ignores_unrelated_run_faults(self):
        with tempfile.TemporaryDirectory() as tmp:
            event_path = os.path.join(tmp, "a5_speed_events.log")
            with open(event_path, "w", encoding="ascii") as stream:
                stream.write("[15:10:14.102] FATAL RECIP_ENCODER_NO_MOTION timeout_s=3.000\n")
            self.assertIsNone(orch._reciprocating_startup_failure_reason(event_path))

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

    def test_final_merge_uses_time_when_full_dlg_and_merge_rates_differ(self):
        with tempfile.TemporaryDirectory() as tmp:
            merged = os.path.join(tmp, "result_T.csv")
            with open(merged, "w", newline="", encoding="utf-8") as f:
                w = csv.writer(f, delimiter=";", lineterminator="\n")
                w.writerow(["idx", "t_s", "PosEncExt"])
                for idx in range(8):
                    w.writerow([idx, idx / 50.0, 10.0 + idx])
            state = SimpleNamespace(
                reciprocating=True,
                merge_csv=merged,
                # Indice pertence ao DLG completo de 200 Hz e nao pode ser
                # comparado diretamente com o indice do merge de 50 Hz.
                reciprocating_final_encoder_idx=20,
                reciprocating_final_encoder_t_s=0.1,
            )
            orch._crop_reciprocating_merge_at_encoder_endpoint(state)
            with open(merged, "r", newline="", encoding="utf-8") as f:
                rows = list(csv.DictReader(f, delimiter=";"))
            self.assertEqual([int(row["idx"]) for row in rows], list(range(6)))

    def test_m_uses_one_target_speed_per_completed_stroke(self):
        with tempfile.TemporaryDirectory() as tmp:
            dlg = os.path.join(tmp, "dlg.csv")
            drive = os.path.join(tmp, "drive.csv")
            encoder = os.path.join(tmp, "encoder_state.csv")
            merged = os.path.join(tmp, "result_T.csv")
            dp = os.path.join(tmp, "result_DP.csv")
            motion = os.path.join(tmp, "result_M.csv")

            with open(dlg, "w", newline="", encoding="ascii") as f:
                w = csv.writer(f, lineterminator="\n")
                w.writerow(["idx", "t_qpc", "t_s", "ch1", "ch2", "ch3", "ch4", "ch5", "ch6", "ch7", "ch8", "atrito", "err"])
                for idx in range(8):
                    force = -1.0 if idx < 4 else 1.0
                    w.writerow([idx, idx, idx * 0.1, force, 0, 0, 0, 0, 0, 0, 0, force, 0])

            positions = [0, 1, 2, 3, 3, 2, 1, 0]
            with open(drive, "w", newline="", encoding="ascii") as f:
                w = csv.writer(f, lineterminator="\n")
                w.writerow(["idx", "t_qpc", "t_s", "cmd_rpm", "cmd_err"])
                for idx in range(8):
                    w.writerow([idx, idx, idx * 0.1, 10 if idx < 4 else -20, 0])

            with open(encoder, "w", newline="", encoding="ascii") as f:
                w = csv.writer(f, lineterminator="\n")
                w.writerow([
                    "idx", "t_qpc", "t_s", "angle_deg", "accepted", "status", "health",
                    "unwrapped_deg", "relative_deg", "relative_mm", "progress_mm", "speed_mm_s",
                    "disk_rpm", "direction", "stationary", "reversal", "extreme_relative_mm",
                    "accepted_age_s",
                ])
                for idx, pos in enumerate(positions):
                    w.writerow([
                        idx, idx, idx * 0.1, pos * 10, 1, "ACCEPTED", "OK", pos * 10,
                        pos * 10, pos, idx, 10, 1 if idx < 4 else -1,
                        1 if idx < 4 else -1, 0, 1 if idx in (3, 7) else 0, pos, 0,
                    ])

            with open(os.path.join(tmp, "a5_speed_events.log"), "w", encoding="ascii") as f:
                f.write("RECIP_ENCODER_TRIGGER idx=3 qpc=3 stroke=1 completed_segment=0\n")
                f.write("RECIP_SHADOW_PHYSICAL_REVERSAL idx=3 qpc=3 stroke=1 endpoint_error_mm=0\n")
                f.write("RECIP_ENCODER_DONE idx=7 qpc=7 strokes=2 completed_segment=1\n")

            state = SimpleNamespace(
                dlg_csv=dlg,
                encoder_state_csv=encoder,
                drive_csv=drive,
                merge_csv=merged,
                turn_dist_csv=dp,
                turn_vp_csv=motion,
                dynamic_offset_n=0.0,
                force_normal_n=1.0,
                reciprocating_course_mm=3.0,
                reciprocating_total_mm=6.0,
                reciprocating_final_encoder_idx=7,
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

            # A media reciprocante por distancia usa o modulo para impedir
            # cancelamento entre valores positivos e negativos. Os extremos
            # preservam o sinal e continuam fisicamente uteis.
            with open(dp, "r", newline="", encoding="utf-8") as f:
                distance_rows = list(csv.DictReader(f, delimiter=";"))
            self.assertEqual(len(distance_rows), 1)
            self.assertEqual(distance_rows[0]["atrito_med"], "1.000000")
            self.assertEqual(distance_rows[0]["atrito_min"], "-1.000000")
            self.assertEqual(distance_rows[0]["atrito_max"], "1.000000")


if __name__ == "__main__":
    unittest.main()
