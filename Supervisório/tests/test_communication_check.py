import ast
import csv
import io
import json
import os
from pathlib import Path
import queue
import struct
import sys
from datetime import datetime
import unittest
from unittest import mock

SUPERVISOR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SUPERVISOR))
import communication_check as comm


class CommunicationTests(unittest.TestCase):
    def dlg_result(self, bad=(), count=600):
        data = io.StringIO()
        writer = csv.writer(data)
        writer.writerow(['idx', 't_s'] + [f'ch{i}' for i in range(1, 9)] + ['err'])
        for index in range(count):
            writer.writerow([index, index / 200] +
                            (['NULL'] * 8 if index in bad else [1] * 8) +
                            [int(index in bad)])
        data.seek(0)
        with mock.patch('builtins.open', return_value=data):
            return comm.analyze_dlg_csv('capture.csv')

    def test_dlg_good_and_one_percent_boundary(self):
        self.assertTrue(self.dlg_result()['ok'])
        self.assertTrue(self.dlg_result(bad=range(6))['ok'])
        self.assertFalse(self.dlg_result(bad=range(7))['ok'])

    def test_dlg_missing_tail_or_only_one_good_packet_cannot_pass(self):
        self.assertFalse(self.dlg_result(count=599)['ok'])
        self.assertFalse(self.dlg_result(count=1)['ok'])
        self.assertFalse(self.dlg_result(count=0)['available'])

    def test_dlg_100ms_outage_is_reported(self):
        result = self.dlg_result(bad=range(120, 140))
        self.assertFalse(result['ok'])
        self.assertEqual(result['max_missing_ms'], 100)

    def test_process_variants_case_and_no_generic_service_match(self):
        found = comm.lynx_assistants(['DLG4KAssistant.exe', 'dlg4kASSIST.EXE',
                                     'LynxAtNet.exe', 'dlg_logger_ipc.exe',
                                     'my_DLG4KAssistant.exe'])
        self.assertEqual(found, ['DLG4KAssistant.exe', 'dlg4kASSIST.EXE'])

    def test_process_scan_failure_is_not_treated_as_clear(self):
        with mock.patch.object(comm, 'list_process_names', side_effect=OSError('scan failed')):
            with self.assertRaises(OSError):
                comm.require_dlg_available()

    def response(self, body):
        return body + struct.pack('<H', comm.crc16(body))

    def read(self, payload, fc=3):
        port = mock.Mock()
        stream = io.BytesIO(payload)
        port.read.side_effect = stream.read
        result = comm.read_register(port, 0x0603, fc)
        request = port.write.call_args.args[0]
        self.assertEqual(request[:6], bytes.fromhex('010306030001') if fc == 3
                         else bytes.fromhex('010406030001'))
        self.assertEqual(comm.crc16(request), 0)
        return result

    def test_modbus_good_and_invalid_packets(self):
        self.assertEqual(self.read(self.response(bytes.fromhex('010302002A')))['value'], 42)
        for response in (b'', b'\x01\x03', bytes.fromhex('010302002A0000'),
                         self.response(bytes.fromhex('020302002A')),
                         self.response(bytes.fromhex('010402002A')),
                         self.response(bytes.fromhex('010304002A')),
                         self.response(bytes.fromhex('018301'))):
            self.assertFalse(self.read(response)['ok'], response)
        with self.assertRaises(ValueError):
            comm.read_register(mock.Mock(), 0x3100, 6)

    def test_drive_never_passes_with_loss_or_excess_latency(self):
        good = [dict(ok=True, elapsed_ms=5) for _ in range(40)]
        self.assertTrue(comm.analyze_drive(good)['ok'])
        self.assertFalse(comm.analyze_drive(good[:-1])['ok'])
        self.assertFalse(comm.analyze_drive(good[:-1] + [dict(ok=False)])['ok'])
        self.assertFalse(comm.analyze_drive([dict(ok=True, elapsed_ms=21)] * 40)['ok'])
        self.assertFalse(comm.analyze_drive(good[:-1] + [dict(ok=True, elapsed_ms=101)])['ok'])
        self.assertFalse(comm.analyze_drive(good[:-3] + [dict(ok=True, elapsed_ms=60)] * 3)['ok'])

    def test_driver_repeats_reads_and_does_not_hide_probe_transport_error(self):
        for error, expected in [('modbus_exception_1', True), ('CRC', False)]:
            probe = [dict(ok=False, error=error), dict(ok=True, elapsed_ms=5),
                     dict(ok=True, elapsed_ms=5)]
            with mock.patch.object(comm.serial, 'Serial'), \
                 mock.patch.object(comm.time, 'sleep'), \
                 mock.patch.object(comm, 'read_register', side_effect=probe +
                                   [dict(ok=True, elapsed_ms=5)] * 40) as read:
                result = comm.check_drive(lambda _: None)
                self.assertEqual(result['ok'], expected)
                self.assertEqual(result['successful'], 40)
                self.assertEqual(read.call_count, 43)
                # Successful fallback is reused; no write function exists.
                self.assertEqual(read.call_args_list[3].args[2], 4)

    def test_assistant_blocks_dlg_but_drive_can_still_be_diagnosed(self):
        with mock.patch.object(comm, 'list_process_names', return_value=['DLG4KAssistant.exe']), \
             mock.patch.object(comm.subprocess, 'Popen') as launch, \
             mock.patch.object(comm, 'check_drive', return_value=dict(ok=True, available=True)):
            result = comm.run_check('logger.exe', 'unused', lambda _: None)
        launch.assert_not_called()
        self.assertFalse(result['ok'])
        self.assertTrue(result['drive']['ok'])
        self.assertIn('Feche o programa', result['dlg']['error'])

    def test_three_independent_dlg_captures_fixed_production_port(self):
        good = dict(ok=True, available=True, valid=600, expected=600,
                    loss_pct=0, max_missing_ms=0)
        proc = mock.Mock()
        proc.returncode = 0
        proc.poll.return_value = 0
        with mock.patch('builtins.open', mock.mock_open()), \
             mock.patch.object(comm.Path, 'mkdir'), \
             mock.patch.object(comm, 'require_dlg_available') as guard, \
             mock.patch.object(comm.subprocess, 'Popen', return_value=proc) as launch, \
             mock.patch.object(comm, 'analyze_dlg_csv', return_value=good.copy()):
            result = comm.check_dlg('logger.exe', 'unused', lambda _: None)
        self.assertTrue(result['ok'])
        self.assertEqual(launch.call_count, 3)
        self.assertEqual(guard.call_count, 3)
        for call in launch.call_args_list:
            cmd = call.args[0]
            self.assertEqual(cmd[cmd.index('--bind-port') + 1], '41402')
            self.assertEqual(cmd[cmd.index('--rate') + 1], '200')
        self.assertEqual(proc.stdin.write.call_count, 3)

    def test_dlg_timeout_requests_stop_and_cannot_pass_valid_csv(self):
        good = dict(ok=True, available=True, valid=600, expected=600,
                    loss_pct=0, max_missing_ms=0)
        proc = mock.Mock()
        proc.returncode = 0
        proc.poll.return_value = None
        proc.wait.side_effect = [comm.subprocess.TimeoutExpired('logger', 15), 0] * 3
        with mock.patch('builtins.open', mock.mock_open()), \
             mock.patch.object(comm.Path, 'mkdir'), \
             mock.patch.object(comm, 'require_dlg_available'), \
             mock.patch.object(comm.subprocess, 'Popen', return_value=proc), \
             mock.patch.object(comm, 'analyze_dlg_csv', side_effect=lambda *_: good.copy()):
            result = comm.check_dlg('logger.exe', 'unused', lambda _: None)
        self.assertFalse(result['ok'])
        self.assertEqual(proc.stdin.write.call_args.args[0], 'STOP\n')


class UIGuardTests(unittest.TestCase):
    def setUp(self):
        tree = ast.parse((SUPERVISOR / 'novo_tribometro.py').read_text(encoding='utf-8-sig'))
        functions = [node for node in tree.body if isinstance(node, ast.FunctionDef)
                     and node.name in {'_check_hardware_busy', '_warn_dlg_interference',
                                       'start_acquisition', '_set_start_button_state',
                                       '_set_status', 'check_status'}]
        self.env = dict(comm_check=comm, messagebox=mock.Mock(), log_msg=mock.Mock(),
                        check_status_running=False, monitor_mode_active=False,
                        tara_running=False, run_finalizing=False, running=False,
                        _external_pipeline_ativo=lambda: False, _is_running=lambda: False,
                        salvar_arquivo=mock.Mock(), _run_auto_tara_before_start=mock.Mock())
        exec(compile(ast.Module(body=functions, type_ignores=[]), '<ui-functions>', 'exec'), self.env)

    def test_start_warns_and_returns_before_any_tara_or_files(self):
        with mock.patch.object(comm, 'list_process_names', return_value=['DLG4KAssistant.exe']):
            self.env['start_acquisition']()
        self.env['messagebox'].showwarning.assert_called_once()
        self.env['salvar_arquivo'].assert_not_called()
        self.env['_run_auto_tara_before_start'].assert_not_called()

    def test_running_check_blocks_start_and_disables_button(self):
        self.env.update(check_status_running=True, button_frame4_iniciar=mock.Mock())
        self.env['start_acquisition']()
        self.env['_set_start_button_state']()
        self.env['button_frame4_iniciar'].config.assert_called_once_with(state='disabled')
        self.env['salvar_arquivo'].assert_not_called()

    def test_check_cannot_interrupt_active_acquisition(self):
        self.env['monitor_mode_active'] = True
        self.env['check_status']()
        self.env['messagebox'].showwarning.assert_called_once()
        self.assertFalse(self.env['check_status_running'])

    def exercise_worker(self, fail=False):
        self.env.update(os=os, json=json, datetime=datetime, queue=queue,
                        APP_SETTINGS_DIR='unused', root=mock.Mock(), orch=mock.Mock(),
                        threading=mock.Mock(), button_frame6_status=mock.Mock(),
                        status_dlg_value=mock.Mock(), status_drive_value=mock.Mock(),
                        status_communication_value=mock.Mock(), status_check_progress=mock.Mock())
        report = dict(ok=True, dlg=dict(ok=True, available=True),
                      drive=dict(ok=True, available=True))
        workers = []
        def launch(*, target, daemon):
            thread = mock.Mock()
            thread.start.side_effect = lambda: workers.append(target)
            return thread
        self.env['threading'].Thread.side_effect = launch
        def check(*args):
            args[2]('Drive: leitura 1/40')
            if fail:
                raise RuntimeError('worker failed')
            return report
        with mock.patch.object(comm, 'require_dlg_available'), \
             mock.patch.object(comm, 'run_check', side_effect=check), \
             mock.patch.object(comm, 'format_report', return_value='summary'), \
             mock.patch.object(os, 'makedirs'), \
             mock.patch('builtins.open', mock.mock_open()):
            self.env['check_status']()
            self.assertTrue(self.env['check_status_running'])
            self.assertEqual(len(workers), 1)
            progress_widget = self.env['status_check_progress']
            calls_before = progress_widget.config.call_count
            poll = self.env['root'].after.call_args.args[1]
            workers[0]()
            # Worker emits queue messages; it never updates widgets itself.
            self.assertEqual(progress_widget.config.call_count, calls_before)
            self.assertTrue(self.env['check_status_running'])
            poll()
        self.assertFalse(self.env['check_status_running'])
        self.env['button_frame6_status'].config.assert_called_with(state='normal')
        self.env['status_communication_value'].config.assert_called_with(
            text='X' if fail else 'OK', fg='red' if fail else 'green')

    def test_worker_progress_and_success_return_through_tk_poll(self):
        self.exercise_worker()
        self.env['messagebox'].showinfo.assert_called_once()

    def test_unexpected_worker_error_releases_busy_state(self):
        self.exercise_worker(fail=True)
        self.env['messagebox'].showerror.assert_called_once()


if __name__ == '__main__':
    unittest.main()
