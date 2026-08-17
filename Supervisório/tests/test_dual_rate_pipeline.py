import os
import csv
import io
import math
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SUPERVISOR_DIR = Path(__file__).resolve().parents[1]
if str(SUPERVISOR_DIR) not in sys.path:
    sys.path.insert(0, str(SUPERVISOR_DIR))

import orchestrator_runtime as orch


class _FakeProcess:
    def __init__(self, cmd, **_kwargs):
        self.cmd = cmd
        self.stdin = mock.Mock()
        self.stdout = mock.Mock()
        self.returncode = None

    def poll(self):
        return None

    def terminate(self):
        self.returncode = 0


class DualRatePipelineTests(unittest.TestCase):
    def test_ui_builds_stage_durations_before_continuous_deadline(self):
        source = (SUPERVISOR_DIR / "novo_tribometro.py").read_text(encoding="utf-8")
        start = source.index("def start_acquisition():")
        end = source.index("\ndef ", start + 1)
        body = source[start:end]
        durations_init = body.index("lista_duracao = []")
        deadline_read = body.index("continuous_theoretical_s = sum(lista_duracao)")
        self.assertLess(durations_init, deadline_read)
        self.assertIn('text="Preparando ensaio"', body[:deadline_read])

    def test_start_uses_dlg_200_drive_50_and_compatibility_csv(self):
        with tempfile.TemporaryDirectory() as tmp:
            paths = {
                "dlg_csv": os.path.join(tmp, "dlg.csv"),
                "drive_csv": os.path.join(tmp, "drive.csv"),
                "turn_dist_csv": os.path.join(tmp, "out_DP.csv"),
                "turn_vp_csv": os.path.join(tmp, "out_VP.csv"),
                "merge_csv": os.path.join(tmp, "out_T.csv"),
                "schedule_csv": os.path.join(tmp, "schedule.csv"),
            }
            created = []

            def fake_popen(cmd, **kwargs):
                proc = _FakeProcess(cmd, **kwargs)
                created.append(proc)
                return proc

            executables = {
                "dlg_exe": "dlg_logger_ipc.exe",
                "drive_exe": "a5_speed_logger.exe",
                "merge_exe": "merge_logs.exe",
                "missing": [],
            }
            with mock.patch.object(orch, "check_executables", return_value=executables), \
                 mock.patch.object(orch.subprocess, "Popen", side_effect=fake_popen), \
                 mock.patch.object(orch, "_wait_ready", return_value=True), \
                 mock.patch.object(orch, "_wait_data_ready", return_value=True), \
                 mock.patch.object(orch, "_send_start"):
                state = orch.start_external_run(
                    repo_root=tmp,
                    out_paths=paths,
                    schedule=[(10, 1.0)],
                    duration_s=1.0,
                )

            self.assertEqual(len(created), 2)
            dlg_cmd = created[0].cmd
            drive_cmd = created[1].cmd
            self.assertEqual(dlg_cmd[dlg_cmd.index("--rate") + 1], "200.000000")
            self.assertEqual(drive_cmd[drive_cmd.index("--rate") + 1], "50.000000")
            self.assertEqual(dlg_cmd[dlg_cmd.index("--compat-rate") + 1], "50.000000")
            self.assertEqual(
                dlg_cmd[dlg_cmd.index("--compat-out") + 1],
                state.dlg_compat_csv,
            )
            self.assertEqual(orch._processing_dlg_csv(state), state.dlg_compat_csv)

    def test_rejects_non_integer_rate_ratio(self):
        with tempfile.TemporaryDirectory() as tmp:
            paths = {
                "dlg_csv": os.path.join(tmp, "dlg.csv"),
                "drive_csv": os.path.join(tmp, "drive.csv"),
                "turn_dist_csv": os.path.join(tmp, "out_DP.csv"),
                "turn_vp_csv": os.path.join(tmp, "out_VP.csv"),
                "merge_csv": os.path.join(tmp, "out_T.csv"),
                "schedule_csv": os.path.join(tmp, "schedule.csv"),
            }
            with self.assertRaisesRegex(ValueError, "multiplo inteiro"):
                orch.start_external_run(
                    repo_root=tmp,
                    out_paths=paths,
                    schedule=[(10, 1.0)],
                    duration_s=1.0,
                    rate_hz=200.0,
                    drive_rate_hz=60.0,
                )

    def test_continuous_encoder_arguments_are_sent_to_dlg_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            paths = {
                "dlg_csv": os.path.join(tmp, "dlg.csv"),
                "drive_csv": os.path.join(tmp, "drive.csv"),
                "turn_dist_csv": os.path.join(tmp, "out_DP.csv"),
                "turn_vp_csv": os.path.join(tmp, "out_VP.csv"),
                "merge_csv": os.path.join(tmp, "out_T.csv"),
                "schedule_csv": os.path.join(tmp, "schedule.csv"),
            }
            created = []
            def fake_popen(cmd, **kwargs):
                proc = _FakeProcess(cmd, **kwargs)
                created.append(proc)
                return proc
            executables = {
                "dlg_exe": "dlg_logger_ipc.exe", "drive_exe": "a5_speed_logger.exe",
                "merge_exe": "merge_logs.exe", "missing": [],
            }
            with mock.patch.object(orch, "check_executables", return_value=executables), \
                 mock.patch.object(orch.subprocess, "Popen", side_effect=fake_popen), \
                 mock.patch.object(orch, "_wait_ready", return_value=True), \
                 mock.patch.object(orch, "_wait_data_ready", return_value=True), \
                 mock.patch.object(orch, "_send_start"), \
                 mock.patch.object(orch.threading.Thread, "start"):
                state = orch.start_external_run(
                    repo_root=tmp, out_paths=paths, schedule=[(20, 10.0)],
                    duration_s=20.0, raio_mm=10.0, relacao=4.0,
                    target_speed_schedule=[(5.0, 10.0)],
                    continuous_target_mm=50.0,
                )
            dlg_cmd, drive_cmd = created[0].cmd, created[1].cmd
            self.assertEqual(dlg_cmd[dlg_cmd.index("--encoder-target-mm") + 1], "50.000000000")
            self.assertEqual(dlg_cmd[dlg_cmd.index("--encoder-radius-mm") + 1], "10.000000000")
            effective_speed = 20.0 * 2.0 * math.pi * 10.0 / (60.0 * 4.0)
            self.assertAlmostEqual(
                float(dlg_cmd[dlg_cmd.index("--encoder-max-speed-mm-s") + 1]),
                2.0 * max(5.0, effective_speed),
                places=8,
            )
            self.assertNotIn("--encoder-target-mm", drive_cmd)
            self.assertNotIn("--encoder-control-port", dlg_cmd)
            self.assertNotIn("--encoder-session", dlg_cmd)
            self.assertNotIn("--self-test-encoder-link", drive_cmd)
            self.assertEqual(state.continuous_encoder_status, "aguardando_alvo")

    def test_reciprocating_shadow_uses_one_session_and_never_replaces_legacy_args(self):
        with tempfile.TemporaryDirectory() as tmp:
            paths = {
                "dlg_csv": os.path.join(tmp, "dlg.csv"),
                "drive_csv": os.path.join(tmp, "drive.csv"),
                "turn_dist_csv": os.path.join(tmp, "out_DP.csv"),
                "turn_vp_csv": os.path.join(tmp, "out_M.csv"),
                "merge_csv": os.path.join(tmp, "out_T.csv"),
                "schedule_csv": os.path.join(tmp, "schedule.csv"),
            }
            created = []

            def fake_popen(cmd, **kwargs):
                proc = _FakeProcess(cmd, **kwargs)
                created.append(proc)
                return proc

            executables = {
                "dlg_exe": "dlg_logger_ipc.exe",
                "drive_exe": "a5_speed_logger.exe",
                "merge_exe": "merge_logs.exe",
                "missing": [],
            }
            with mock.patch.object(orch, "check_executables", return_value=executables), \
                 mock.patch.object(orch.subprocess, "Popen", side_effect=fake_popen), \
                 mock.patch.object(orch, "_wait_ready", return_value=True), \
                 mock.patch.object(orch, "_wait_data_ready", return_value=True), \
                 mock.patch.object(orch, "_send_start"), \
                 mock.patch.object(orch, "_reserve_loopback_udp_port", return_value=54321), \
                 mock.patch.object(orch.secrets, "randbits", return_value=123456789):
                state = orch.start_external_run(
                    repo_root=tmp,
                    out_paths=paths,
                    schedule=[(4, 30.0)],
                    duration_s=30.0,
                    relacao=4.0,
                    raio_mm=10.0,
                    reciprocating=True,
                    reciprocating_course_mm=10.0,
                    reciprocating_total_mm=100.0,
                    reciprocating_tolerance_counts=20,
                    reciprocating_shadow_enabled=True,
                    reciprocating_shadow_forward_sign=-1,
                    reciprocating_shadow_stop_compensation=True,
                    target_speed_schedule=[(1.0, 30.0)],
                )

            dlg_cmd, drive_cmd = created[0].cmd, created[1].cmd
            self.assertIn("--encoder-reciprocating-shadow", dlg_cmd)
            self.assertEqual(dlg_cmd[dlg_cmd.index("--encoder-control-port") + 1], "54321")
            self.assertEqual(dlg_cmd[dlg_cmd.index("--encoder-session") + 1], "123456789")
            self.assertEqual(dlg_cmd[dlg_cmd.index("--encoder-target-mm") + 1], "0")
            self.assertIn("--reciprocating", drive_cmd)
            self.assertIn("--recip-encoder-shadow", drive_cmd)
            self.assertEqual(drive_cmd[drive_cmd.index("--recip-shadow-port") + 1], "54321")
            self.assertEqual(drive_cmd[drive_cmd.index("--recip-shadow-session") + 1], "123456789")
            self.assertEqual(drive_cmd[drive_cmd.index("--recip-shadow-forward-sign") + 1], "-1")
            self.assertEqual(drive_cmd[drive_cmd.index("--recip-total-mm") + 1], "100.000000")
            self.assertEqual(drive_cmd[drive_cmd.index("--recip-shadow-total-mm") + 1], "100.000000000")
            self.assertIn("--recip-shadow-stop-compensation", drive_cmd)
            self.assertEqual(
                drive_cmd[drive_cmd.index("--recip-shadow-stop-slope-s") + 1],
                "0.131998358322",
            )
            self.assertEqual(
                drive_cmd[drive_cmd.index("--recip-shadow-stop-margin-mm") + 1],
                "0.270250016300",
            )
            self.assertEqual(
                drive_cmd[drive_cmd.index("--recip-shadow-stop-velocity-window-s") + 1],
                "0.250000000",
            )
            self.assertEqual(
                drive_cmd[drive_cmd.index("--recip-shadow-stop-max-course-fraction") + 1],
                "0.450000000",
            )
            self.assertTrue(state.reciprocating_shadow_enabled)
            self.assertTrue(state.reciprocating_shadow_stop_compensation)
            self.assertEqual(state.reciprocating_shadow_session, 123456789)
            self.assertEqual(state.reciprocating_shadow_forward_sign, -1)

    def test_reciprocating_shadow_receiver_contains_no_drive_actuation_calls(self):
        source = (
            SUPERVISOR_DIR.parent / "DriveA5" / "src" / "a5_speed_logger.c"
        ).read_text(encoding="utf-8")
        start = source.index("static void recip_shadow_drain(")
        end = source.index("static void recip_shadow_close(", start)
        body = source[start:end]
        for forbidden in (
            "cmd_rpm(", "cmd_run(", "cmd_stop(", "cmd_vdi_stop(",
            "stop_drive_now(", "recip_reverse(",
        ):
            self.assertNotIn(forbidden, body)
        self.assertIn("recip_shadow_log_decision", body)

    def test_reciprocating_encoder_control_disables_drive_telemetry(self):
        with tempfile.TemporaryDirectory() as tmp:
            paths = {
                "dlg_csv": os.path.join(tmp, "dlg.csv"),
                "drive_csv": os.path.join(tmp, "drive.csv"),
                "turn_dist_csv": os.path.join(tmp, "out_DP.csv"),
                "turn_vp_csv": os.path.join(tmp, "out_M.csv"),
                "merge_csv": os.path.join(tmp, "out_T.csv"),
                "schedule_csv": os.path.join(tmp, "schedule.csv"),
            }
            created = []

            def fake_popen(cmd, **kwargs):
                proc = _FakeProcess(cmd, **kwargs)
                created.append(proc)
                return proc

            executables = {
                "dlg_exe": "dlg_logger_ipc.exe",
                "drive_exe": "a5_speed_logger.exe",
                "merge_exe": None,
                "missing": [],
            }
            with mock.patch.object(orch, "check_executables", return_value=executables), \
                 mock.patch.object(orch.subprocess, "Popen", side_effect=fake_popen), \
                 mock.patch.object(orch, "_wait_ready", return_value=True), \
                 mock.patch.object(orch, "_wait_data_ready", return_value=True), \
                 mock.patch.object(orch, "_send_start"), \
                 mock.patch.object(orch, "_reserve_loopback_udp_port", return_value=54321), \
                 mock.patch.object(orch.secrets, "randbits", return_value=123456789):
                state = orch.start_external_run(
                    repo_root=tmp,
                    out_paths=paths,
                    schedule=[(20, 30.0)],
                    duration_s=30.0,
                    relacao=4.0,
                    raio_mm=10.0,
                    reciprocating=True,
                    reciprocating_course_mm=10.0,
                    reciprocating_total_mm=100.0,
                    reciprocating_tolerance_counts=20,
                    reciprocating_encoder_control_enabled=True,
                    reciprocating_shadow_forward_sign=1,
                    reciprocating_shadow_stop_compensation=True,
                    drive_command_only=True,
                    target_speed_schedule=[(5.0, 30.0)],
                )

            dlg_cmd, drive_cmd = created[0].cmd, created[1].cmd
            self.assertIn("--encoder-control-port", dlg_cmd)
            self.assertNotIn("--compat-out", dlg_cmd)
            self.assertIn("--recip-encoder-control", drive_cmd)
            self.assertIn("--command-only", drive_cmd)
            self.assertNotIn("--recip-encoder-shadow", drive_cmd)
            self.assertTrue(state.reciprocating_encoder_control_enabled)
            self.assertTrue(state.drive_command_only)
            self.assertEqual(state.dlg_compat_csv, state.dlg_csv)

    def test_shadow_summary_compares_external_proposals_to_legacy_triggers(self):
        with tempfile.TemporaryDirectory() as tmp:
            dev = os.path.join(tmp, "DadosDev")
            os.makedirs(dev)
            event_path = os.path.join(dev, "a5_speed_events.log")
            with open(event_path, "w", encoding="ascii") as stream:
                stream.write("[00:00:01.000] RECIP_STOP_TRIGGER idx=100 dir=1\n")
                stream.write("[00:00:01.010] RECIP_SHADOW_REVERSE idx=102 action_enabled=0\n")
                stream.write("[00:00:02.000] RECIP_STOP_TRIGGER idx=200 dir=-1\n")
                stream.write("[00:00:02.010] RECIP_SHADOW_REVERSE idx=199 action_enabled=0\n")
                stream.write("[00:00:03.000] RECIP_DONE idx=300\n")
                stream.write("[00:00:03.010] RECIP_SHADOW_COMPLETE idx=303 action_enabled=0\n")
                stream.write(
                    "[00:00:03.020] RECIP_SHADOW_END valid=600 rejected=0 "
                    "sequence_gaps=0 complete=1 fault=NONE action_enabled=0\n"
                )
            state = type("State", (), {
                "reciprocating_shadow_enabled": True,
                "merge_csv": os.path.join(tmp, "out_T.csv"),
                "drive_rate_hz": 50.0,
                "reciprocating_shadow_summary": None,
            })()
            summary = orch.load_reciprocating_shadow_summary(state)
            self.assertEqual(summary["comparison_pairs"], "2")
            self.assertEqual(summary["reverse_delta_mean_ms"], "10.000000")
            self.assertEqual(summary["reverse_delta_min_ms"], "-20.000000")
            self.assertEqual(summary["reverse_delta_max_ms"], "40.000000")
            self.assertEqual(summary["completion_delta_ms"], "60.000000")
            self.assertEqual(summary["fault"], "NONE")

    def test_stop_diagnostics_consolidates_one_completed_stroke(self):
        with tempfile.TemporaryDirectory() as tmp:
            event_path = os.path.join(tmp, "a5_speed_events.log")
            drive_path = os.path.join(tmp, "drive.csv")
            encoder_path = os.path.join(tmp, "encoder_state.csv")
            output_path = os.path.join(tmp, "recip_stop_diagnostics.csv")
            with open(event_path, "w", encoding="ascii") as stream:
                stream.write("[00:00:02.000] RECIP_STOP_TRIGGER idx=100 dir=1 target_rpm_abs=20\n")
                stream.write("[00:00:02.020] RECIP_SHADOW_REVERSE idx=101 qpc=2020000 stroke=1 new_direction=-1 position_mm=9.8 observation_latency_ms=3.5 action_enabled=0\n")
                stream.write("[00:00:02.100] RECIP_SHADOW_PHYSICAL_REVERSAL idx=105 qpc=2100000 stroke=1 direction=-1 extreme_mm=10.4 observation_latency_ms=4.0 action_enabled=0\n")
                stream.write("[00:00:02.120] RECIP_REVERSE idx=106 completed_segment=0 target_rpm_abs=20\n")
            with open(drive_path, "w", newline="", encoding="ascii") as stream:
                writer = csv.writer(stream, lineterminator="\n")
                writer.writerow(["idx", "t_qpc", "t_s"])
                writer.writerow([100, 2000000, "2.000000"])
            with open(encoder_path, "w", newline="", encoding="ascii") as stream:
                writer = csv.writer(stream, lineterminator="\n")
                writer.writerow(["idx", "t_qpc", "t_s", "relative_mm", "accepted"])
                for idx in range(251):
                    t_s = idx / 100.0
                    writer.writerow([idx, idx * 10000, f"{t_s:.6f}", f"{5.0 * t_s:.9f}", 1])
            state = type("State", (), {
                "reciprocating_shadow_enabled": True,
                "merge_csv": os.path.join(tmp, "out_T.csv"),
                "drive_csv": drive_path,
                "encoder_state_csv": encoder_path,
                "target_speed_schedule": [(5.0, 10.0)],
                "raio_mm": 10.0,
                "relacao": 4.0,
                "reciprocating_course_mm": 10.0,
                "reciprocating_stop_diagnostics": None,
            })()
            summary = orch.build_reciprocating_stop_diagnostics(
                state, output_path=output_path, phase="teste"
            )
            self.assertEqual(summary["rows"], 1)
            self.assertEqual(summary["complete_rows"], 1)
            with open(output_path, newline="", encoding="ascii") as stream:
                rows = list(csv.DictReader(stream, delimiter=";"))
            self.assertEqual(rows[0]["fase"], "teste")
            self.assertAlmostEqual(float(rows[0]["velocidade_externa_modulo_mm_s"]), 5.0, places=6)
            self.assertAlmostEqual(float(rows[0]["distancia_parada_apos_gatilho_legado_mm"]), 0.4, places=6)
            self.assertAlmostEqual(float(rows[0]["tempo_parada_apos_gatilho_legado_ms"]), 100.0, places=6)
            self.assertAlmostEqual(float(rows[0]["defasagem_externo_legado_ms"]), 20.0, places=6)
            self.assertEqual(rows[0]["latencia_observacao_externo_ms"], "3.500000")

    def test_continuous_outputs_use_full_rate_encoder_data(self):
        with tempfile.TemporaryDirectory() as tmp:
            dlg = os.path.join(tmp, "dlg.csv")
            enc = os.path.join(tmp, "encoder_state.csv")
            result = os.path.join(tmp, "out_T.csv")
            dp = os.path.join(tmp, "out_DP.csv")
            vp = os.path.join(tmp, "out_VP.csv")
            radius = 10.0
            speed = 5.0
            duration = 14.0
            count = int(duration * 200) + 1
            with open(dlg, "w", newline="", encoding="ascii") as fdlg, \
                 open(enc, "w", newline="", encoding="ascii") as fenc:
                wd = csv.writer(fdlg, lineterminator="\n")
                we = csv.writer(fenc, lineterminator="\n")
                wd.writerow(["idx","t_qpc","t_s","ch1","ch2","ch3","ch4","ch5","ch6","ch7","ch8","atrito","err"])
                we.writerow(["idx","t_qpc","t_s","angle_deg","accepted","status","health","unwrapped_deg","relative_deg","relative_mm","progress_mm","speed_mm_s","disk_rpm","direction","stationary","reversal","extreme_relative_mm","accepted_age_s"])
                for idx in range(count):
                    t_s = idx / 200.0
                    progress = speed * t_s
                    angle = progress * 360.0 / (2.0 * math.pi * radius)
                    rpm = speed * 60.0 / (2.0 * math.pi * radius)
                    wd.writerow([idx, idx * 5000, f"{t_s:.6f}", 2,0,angle,0,0,0,0,0,0.2,0])
                    we.writerow([idx,idx*5000,f"{t_s:.6f}",angle,1,"ACCEPTED","OK",angle,angle,progress,progress,speed,rpm,1,0,0,0,0])
            state = type("State", (), {
                "dlg_csv": dlg, "encoder_state_csv": enc, "merge_csv": result,
                "turn_dist_csv": dp, "turn_vp_csv": vp, "raio_mm": radius,
                "distance_interval_mm": 10.0, "force_normal_n": 10.0,
                "encoder_quarantine_samples": 0, "encoder_quarantine_fraction": 0.0,
            })()
            orch._build_continuous_encoder_outputs(state)
            with open(result, newline="", encoding="utf-8") as f:
                result_rows = list(csv.DictReader(f, delimiter=";"))
            with open(dp, newline="", encoding="utf-8") as f:
                dp_rows = list(csv.DictReader(f, delimiter=";"))
            with open(vp, newline="", encoding="utf-8") as f:
                vp_rows = list(csv.DictReader(f, delimiter=";"))
            self.assertEqual(len(result_rows), count)
            self.assertEqual(len(dp_rows), 7)
            self.assertEqual(len(vp_rows), 1)
            self.assertTrue(all(abs(float(row["velocidade_media_mm_s"]) - speed) < 1e-9 for row in dp_rows))
            self.assertAlmostEqual(float(result_rows[-1]["pos"]), speed * duration)
            self.assertNotIn("drive_pos_err", result_rows[0])

    def test_continuous_direction_uses_robust_net_displacement(self):
        tracker = orch._ContinuousDirectionTracker()
        previous_progress = 0.0
        for idx in range(600):
            t_s = idx / 200.0
            relative = t_s + (0.035 if idx % 2 else -0.035)
            if 350 <= idx < 360:
                relative -= 3.0
            progress = tracker.update(t_s, relative, True)
            self.assertGreaterEqual(progress, previous_progress)
            previous_progress = progress
        self.assertEqual(tracker.direction, 1)
        self.assertIsNone(tracker.fault)
        self.assertGreaterEqual(tracker.lock_t_s, 1.0)
        self.assertLess(tracker.lock_t_s, 1.3)
        self.assertGreater(previous_progress, 2.9)

    def test_continuous_failed_run_replay_locks_and_reaches_target(self):
        encoder_path = Path(
            r"C:\Users\nicol\Desktop\Repositorio\2026-08-17 - PoD - "
            r"TesteEncoder_3-1\DadosDev\encoder_state.csv"
        )
        if not encoder_path.is_file():
            self.skipTest("replay real do continuo de 2026-08-17 indisponivel")
        tracker = orch._ContinuousDirectionTracker()
        crossing_t = None
        with encoder_path.open(newline="", encoding="utf-8") as stream:
            for row in csv.DictReader(stream):
                t_s = float(row["t_s"])
                relative = None if row["relative_mm"] == "NULL" else float(row["relative_mm"])
                progress = tracker.update(t_s, relative, row["accepted"] == "1" and relative is not None)
                if crossing_t is None and progress >= 660.0:
                    crossing_t = t_s
        self.assertEqual(tracker.direction, 1)
        self.assertIsNone(tracker.fault)
        self.assertGreaterEqual(tracker.lock_t_s, 2.5)
        self.assertLessEqual(tracker.lock_t_s, 2.7)
        self.assertAlmostEqual(crossing_t, 240.36, places=2)

    def test_target_event_stops_both_processes(self):
        state = type("State", (), {})()
        state.dlg_proc = _FakeProcess([])
        state.drive_proc = _FakeProcess([])
        state.dlg_proc.stdout = io.StringIO(
            "ENCODER_TARGET_REACHED t_qpc=10 t_s=1.25 target_mm=5 progress_mm=5 direction=1\n"
        )
        state.continuous_encoder_status = "aguardando_alvo"
        state.continuous_encoder_target_t_s = None
        state.continuous_encoder_message = ""
        with mock.patch.object(orch, "_send_ipc") as send:
            orch._monitor_continuous_encoder_events(state)
        self.assertEqual(state.continuous_encoder_status, "alvo_atingido")
        self.assertEqual(state.continuous_encoder_target_t_s, 1.25)
        self.assertEqual(send.call_count, 2)

    def test_direction_timeout_event_has_specific_status(self):
        state = type("State", (), {})()
        state.dlg_proc = _FakeProcess([])
        state.drive_proc = _FakeProcess([])
        state.dlg_proc.stdout = io.StringIO(
            "ENCODER_FAILED t_qpc=10 t_s=4.2 accepted_age_s=0 status=ACCEPTED "
            "reason=DIRECTION_LOCK_TIMEOUT\n"
        )
        state.continuous_encoder_status = "aguardando_alvo"
        state.continuous_encoder_message = ""
        with mock.patch.object(orch, "_send_ipc") as send:
            orch._monitor_continuous_encoder_events(state)
        self.assertEqual(state.continuous_encoder_status, "falha_sentido_timeout")
        self.assertEqual(send.call_count, 2)

    def test_manual_stop_is_not_reported_as_encoder_deadline(self):
        state = type("State", (), {})()
        state.dlg_proc = _FakeProcess([])
        state.drive_proc = _FakeProcess([])
        state.dlg_proc.stdout = io.StringIO("")
        state.continuous_encoder_status = "aguardando_alvo"
        state.continuous_encoder_message = ""
        state.manual_stop_requested = True
        with mock.patch.object(orch, "_send_ipc"):
            orch._monitor_continuous_encoder_events(state)
        self.assertEqual(state.continuous_encoder_status, "parada_manual")


if __name__ == "__main__":
    unittest.main()
