"""Verificacoes do contrato externo e dos arquivos; nenhuma COM/DLG real."""
import csv
import importlib
import json
import math
from pathlib import Path
import struct
import sys
import tempfile
import time
import unittest
from unittest import mock
import zlib

SUPERVISOR = Path(__file__).resolve().parents[1]
ROOT = SUPERVISOR.parent
sys.path.insert(0, str(SUPERVISOR))
sys.path.insert(0, str(ROOT/'sinewave'))
import curve_program
import sinewave_integration as integration
import sinewave_protocol as protocol
import orchestrator_runtime as orch


class SinewaveIntegrationTests(unittest.TestCase):
    def request(self, folder, cycles=1, frequency=4):
        request = protocol.make_request(4, frequency, cycles, 10, 4,
                                        Path(folder)/'DadosDev', True)
        request.update(force_normal_n=10, distance_interval_mm=1)
        return request

    def command(self):
        return [sys.executable, str(ROOT/'sinewave/sinewave_runtime.py')]

    def launch(self, folder, request):
        paths = {'merge_csv':str(Path(folder)/'test_T.csv'),
                 'turn_dist_csv':str(Path(folder)/'test_DP.csv'),
                 'turn_vp_csv':str(Path(folder)/'test_M.csv')}
        with mock.patch.object(integration, 'controller_command', return_value=self.command()):
            return integration.start_run(str(ROOT), paths, request)

    def test_physical_units_and_invalid_requests(self):
        r = self.request(ROOT/'out', frequency=.1)
        self.assertAlmostEqual(protocol.derived(r)['peak_motor_rpm'], 4.8)
        for bad in (0, -1, float('nan'), float('inf'), True):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                protocol.make_request(bad,.1,1,10,4,ROOT/'out')
        with self.assertRaises(ValueError):
            protocol.make_request(4,.1,1.5,10,4,ROOT/'out')

    def test_hardware_plan_is_rejected_before_motor_import(self):
        with tempfile.TemporaryDirectory() as tmp:
            r=self.request(tmp); r['simulation']=False
            with self.assertRaisesRegex(RuntimeError,'dois sentidos'):
                integration.validate_controller(self.command(),r,Path(tmp)/'request.json')
            self.assertFalse((Path(tmp)/'DadosDev').exists())

    def test_motor_import_has_no_serial_side_effect(self):
        with mock.patch('pymodbus.client.ModbusSerialClient') as serial:
            motor=importlib.import_module('motor')
            importlib.reload(motor)
            serial.assert_not_called()

    def test_motor_applies_slave_and_rejects_stop_errors(self):
        import motor
        client=mock.Mock()
        client.connect.return_value=True
        with mock.patch.object(motor,'ModbusSerialClient',return_value=client):
            obj=motor.Motor('COM99',115200,'E',7)
            client.write_register.return_value.isError.return_value=False
            obj.start()
            client.write_register.assert_called_with(motor.ADDR_VDI_LEVEL,3,device_id=7)
            client.write_register.return_value.isError.return_value=True
            with self.assertRaisesRegex(RuntimeError,'STOP nao confirmado'):
                obj.stop()
            obj.close()

    def test_complete_run_uses_encoder_for_all_processed_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            state=self.launch(tmp,self.request(tmp,cycles=4))
            self.assertEqual(orch.wait_and_merge(state),0)
            self.assertEqual(state.reciprocating_processing_summary['strokes'],8)
            self.assertAlmostEqual(state.reciprocating_processing_summary['physical_distance_mm'],32)
            with open(state.turn_vp_csv,newline='') as stream:
                rows=list(csv.DictReader(stream,delimiter=';'))
            self.assertTrue(all(r['VELOCIDADE_ALVO']=='NULL' for r in rows))
            with open(state.dlg_csv) as stream:
                dlg=list(csv.DictReader(stream))
            with open(state.encoder_state_csv) as stream:
                enc=list(csv.DictReader(stream))
            self.assertEqual([(r['idx'],r['t_qpc']) for r in dlg],
                             [(r['idx'],r['t_qpc']) for r in enc])
            self.assertFalse(Path(state.drive_csv).exists())  # Nada de telemetria inventada.
            integration.write_info(Path(tmp)/'test_I.csv',state,{'Ensaio':'contrato'})
            self.assertIn('SIMULACAO SEM HARDWARE;SIM;', (Path(tmp)/'test_I.csv').read_text())

    def test_partial_stop_preserves_t_without_inventing_completed_strokes(self):
        with tempfile.TemporaryDirectory() as tmp:
            state=self.launch(tmp,self.request(tmp,cycles=4,frequency=.5))
            time.sleep(.08)
            orch.stop_run(state,manual=True)
            self.assertEqual(orch.wait_and_merge(state),0)
            self.assertEqual(state.controller_client.stopped['reason'],'operator')
            self.assertEqual(state.reciprocating_processing_summary['strokes'],0)
            with open(state.merge_csv) as stream:
                rows=list(csv.DictReader(stream,delimiter=';'))
            self.assertGreater(len(rows),0)

    def test_stop_before_start_produces_no_data(self):
        with tempfile.TemporaryDirectory() as tmp:
            r=self.request(tmp); Path(r['output']['directory']).mkdir()
            cfg=Path(tmp)/'request.json'; cfg.write_text(json.dumps(r))
            client=integration.ControllerClient(self.command(),cfg,r)
            try:
                client.wait_event('ready')
                client.send('stop',reason='operator')
                client.proc.wait(timeout=5)
            finally:
                client.close()
            self.assertIsNotNone(client.stopped)
            self.assertFalse((Path(r['output']['directory'])/'dlg.csv').exists())
            self.assertFalse(any(e['type']=='started' for e in client.events))

    def test_eof_cancels_without_start(self):
        with tempfile.TemporaryDirectory() as tmp:
            r=self.request(tmp); Path(r['output']['directory']).mkdir()
            cfg=Path(tmp)/'request.json'; cfg.write_text(json.dumps(r))
            client=integration.ControllerClient(self.command(),cfg,r)
            client.wait_event('ready')
            client.proc.stdin.close()
            client.proc.wait(timeout=5)
            client.close()
            self.assertEqual(client.stopped['reason'],'stdin_eof')
            self.assertFalse(any(e['type']=='started' for e in client.events))

    def test_wrong_session_is_failure_and_never_starts(self):
        with tempfile.TemporaryDirectory() as tmp:
            r=self.request(tmp); Path(r['output']['directory']).mkdir()
            cfg=Path(tmp)/'request.json'; cfg.write_text(json.dumps(r))
            client=integration.ControllerClient(self.command(),cfg,r)
            client.wait_event('ready')
            client.proc.stdin.write(json.dumps({'schema_version':1,'run_id':'OUTRA','type':'start'})+'\n')
            client.proc.stdin.flush()
            client.proc.wait(timeout=5); client.close()
            self.assertTrue(client.failure)
            self.assertFalse(any(e['type']=='started' for e in client.events))

    def test_encoder_packet_matches_c_contract_and_rejects_crc_session_stale(self):
        # Mesmos valores do fixture encoder_control_protocol_tests.c.
        values=[0x31434e45,1,92,123,7,456,12.5,15.,-2.5,13.,.005,-1,1,0,11,0]
        raw=protocol.ENCODER_PACKET.pack(*values)
        raw=raw[:88]+struct.pack('<I',zlib.crc32(raw[:88]))
        got=protocol.decode_encoder(raw,123,6,506,1000)
        self.assertAlmostEqual(got['source_age_s'],.055)
        self.assertTrue(got['accepted'])
        for payload,session,seq,now in ((raw,124,6,506),(raw,123,7,506),
                                       (raw,123,6,455),(raw,123,6,600),
                                       (raw[:-1],123,6,506),
                                       (raw[:40]+bytes([raw[40]^1])+raw[41:],123,6,506)):
            with self.subTest(session=session,seq=seq,now=now),self.assertRaises(ValueError):
                protocol.decode_encoder(payload,session,seq,now,1000)


if __name__=='__main__':
    unittest.main()
