"""Production logger integration over 127.0.0.1 only; no DLG/COM hardware.

Archives are kept under out/fifo_fix_20261027 for review. Calibration is
isolated with a child-only LOCALAPPDATA, never the operator's calibration.
"""
import csv
import ctypes
import json
import math
import os
from pathlib import Path
import queue
import socket
import struct
import subprocess
import threading
import time
import unittest
import uuid
import zlib

ROOT = Path(__file__).resolve().parents[2]
EXE = ROOT / 'DLG4000/bin/Release/dlg_logger_ipc.exe'
ARTIFACTS = ROOT / 'out/fifo_fix_20261027/integration'
HEADER = struct.Struct('<HHiifhhiii')
CONTROL = struct.Struct('<IHHQQq5di4I')


def qpc():
    value = ctypes.c_longlong()
    ctypes.windll.kernel32.QueryPerformanceCounter(ctypes.byref(value))
    return value.value


class FakeDLG:
    def __init__(self, scenario='normal'):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind(('127.0.0.1', 0))
        self.socket.setblocking(False)
        self.port = self.socket.getsockname()[1]
        self.scenario = scenario
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.error = None

    def run(self):
        target = None
        origin = None
        frame = 0
        previous = None
        source_rate = 198.0 if self.scenario == 'reported_rate' else 200.0
        try:
            while not self.stop.is_set():
                try:
                    data, address = self.socket.recvfrom(2048)
                    opcode = struct.unpack_from('<H', data)[0]
                    if opcode == 0x1002:
                        target, origin, frame = address, time.perf_counter(), 0
                    elif opcode == 0x1006:
                        origin = None
                except BlockingIOError:
                    pass
                except ConnectionResetError:
                    # Windows reports the ICMP response from a logger that has
                    # already closed its ephemeral UDP port at capture end.
                    pass
                if origin is not None and self.scenario != 'silent':
                    now = time.perf_counter()
                    while now >= origin + (frame*3+2)/source_rate:
                        values = []
                        for i in range(3):
                            t = (frame*3+i)/source_rate
                            position = 5*t
                            angle = 100 + position*360/(2*math.pi*10)
                            values.extend([frame, 0, round(angle/.05), 0, 0, 0, 0, 0])
                        payload = HEADER.pack(0x100B,0,frame,frame*15,source_rate,3,8,0,0,0)
                        payload += struct.pack('<24h',*values)
                        if self.scenario != 'loss' or frame not in range(25,29):
                            self.socket.sendto(payload,target)
                        if self.scenario == 'duplicates' and frame == 25:
                            self.socket.sendto(payload,target)
                            if previous is not None:
                                self.socket.sendto(previous,target)
                            self.socket.sendto(payload[:12],target)
                            self.socket.sendto(payload+bytes(1600),target)
                        previous = payload
                        frame += 1
                self.stop.wait(.001)
        except Exception as error:
            self.error = error

    def __enter__(self):
        # Match the production logger's 1 ms timer resolution. On Windows the
        # request is per process; without it this emulator batches artificial
        # 15.6 ms scheduler ticks into packets that were meant to arrive at 15 ms.
        ctypes.windll.winmm.timeBeginPeriod(1)
        self.thread.start()
        return self

    def __exit__(self, *_args):
        self.stop.set()
        self.thread.join(2)
        self.socket.close()
        ctypes.windll.winmm.timeEndPeriod(1)
        if self.error:
            raise self.error


@unittest.skipUnless(os.name=='nt' and EXE.exists(), 'Windows production logger must be built')
class EncoderStreamIntegrationTests(unittest.TestCase):
    def capture(self, scenario, duration=1.5, stop_command=None):
        folder = ARTIFACTS / (scenario+'_'+uuid.uuid4().hex[:8])
        profile = folder/'profile/LATRIB/calibrations'
        profile.mkdir(parents=True)
        calibration = dict(schema_version=1,purpose='encoder_ch3_angle_deg',unit='deg',
                           channels=[3],tSensor=1,iGain=1,iLPF=0,iSensPwr=1,
                           fInputDCImp=1,fInputACImp=0,complete_revolutions=10,
                           fit=dict(slope=.05,intercept=0),quality=dict(accepted=True))
        (profile/'encoder_external_ch3.json').write_text(json.dumps(calibration,indent=2),encoding='ascii')
        receiver=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        receiver.bind(('127.0.0.1',0))
        receiver.settimeout(.05)
        stop_receive=threading.Event()
        control=[]
        def receive():
            while not stop_receive.is_set():
                try:
                    payload,_=receiver.recvfrom(2048)
                except socket.timeout:
                    continue
                control.append((qpc(),payload))
        recv_thread=threading.Thread(target=receive,daemon=True)
        recv_thread.start()
        environment=dict(os.environ,LOCALAPPDATA=str(folder/'profile'))
        with FakeDLG(scenario) as fake:
            command=[str(EXE),'--out',str(folder/'dlg.csv'),'--duration',str(duration),
                     '--rate','200','--ip','127.0.0.1','--port',str(fake.port),
                     '--bind-ip','127.0.0.1','--bind-port','0',
                     '--encoder-state-out',str(folder/'encoder_state.csv'),
                     '--encoder-radius-mm','10','--encoder-max-speed-mm-s','40',
                     '--encoder-target-mm','0','--encoder-control-port',str(receiver.getsockname()[1]),
                     '--encoder-session','123','--encoder-reciprocating-shadow',
                     '--encoder-acquisition-clock','--ipc']
            process=subprocess.Popen(command,cwd=str(folder),env=environment,
                stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
                text=True,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            lines=[]
            output=queue.Queue()
            def reader():
                for line in process.stdout:
                    lines.append(line)
                    output.put(line)
            reader_thread=threading.Thread(target=reader,daemon=True)
            reader_thread.start()
            try:
                deadline=time.monotonic()+4
                while True:
                    line=output.get(timeout=max(.01,deadline-time.monotonic()))
                    if line.strip()=='READY':break
                    if time.monotonic()>deadline:raise AssertionError('READY timeout')
                process.stdin.write('START\nRESUME\nSTOP\n' if stop_command=='BATCH' else 'START\n')
                process.stdin.flush()
                if stop_command and stop_command!='BATCH':
                    time.sleep(.2)
                    if stop_command=='EOF':process.stdin.close()
                    else:process.stdin.write(stop_command+'\n');process.stdin.flush()
                rc=process.wait(timeout=8)
                reader_thread.join(2)
            finally:
                if process.poll() is None:
                    if not process.stdin.closed:
                        process.stdin.write('STOP\n');process.stdin.flush()
                    try:process.wait(timeout=2)
                    except subprocess.TimeoutExpired:process.kill();process.wait()
                process.stdin.close();process.stdout.close()
                stop_receive.set();recv_thread.join(1);receiver.close()
        (folder/'console.txt').write_text(''.join(lines),encoding='ascii',errors='replace')
        def rows(name):
            with (folder/name).open(newline='') as stream:return list(csv.DictReader(stream))
        return folder,rc,lines,control,rows('dlg.csv'),rows('encoder_state.csv'),rows('dlg_acquisition.csv')

    def assert_aligned(self, data, encoder):
        self.assertEqual(len(data),len(encoder))
        previous=0
        for index,(d,e) in enumerate(zip(data,encoder)):
            self.assertEqual(int(d['idx']),index)
            self.assertEqual(d['idx'],e['idx'])
            self.assertEqual(d['t_qpc'],e['t_qpc'])
            self.assertEqual(d['t_s'],e['t_s'])
            self.assertGreater(int(d['t_qpc']),previous)
            previous=int(d['t_qpc'])
            if d['ch3']!='NULL':self.assertEqual(d['ch3'],e['angle_deg'])

    def test_production_logger_pairs_and_source_age(self):
        folder,rc,lines,control,data,encoder,timing=self.capture('normal')
        self.assertEqual(rc,0,folder)
        self.assertIn('DATA_OK\n',lines)
        self.assertEqual(len(data),300)
        self.assert_aligned(data,encoder)
        self.assertGreater(len(control),250)
        self.assertTrue(all(len(p)==92 and zlib.crc32(p[:-4])==struct.unpack_from('<I',p,88)[0] for _,p in control))
        actual=[r for r in timing if r['err']=='0']
        self.assertGreater(len(actual),280)
        self.assertLess(max(float(r['source_age_ms']) for r in actual),100)
        self.assertTrue(all(int(r['source_qpc'])<=int(r['receive_qpc'])<=int(r['publish_qpc']) for r in actual))
        self.assertTrue(all(r['status']=='ACCEPTED' for r in encoder if r['accepted']=='1'))

    def test_packet_loss_is_null_without_pair_misalignment(self):
        folder,rc,_,_,data,encoder,timing=self.capture('loss')
        self.assertEqual(rc,0,folder)
        self.assert_aligned(data,encoder)
        self.assertGreaterEqual(sum(r['err']=='1' for r in data),10)
        self.assertEqual(sum(r['err']=='1' for r in data),sum(r['err']=='1' for r in timing))
        for d,e in zip(data,encoder):
            if d['err']=='1':self.assertEqual(e['accepted'],'0')

    def test_reported_hardware_rate_drives_sample_spacing(self):
        folder,rc,_,_,data,encoder,timing=self.capture('reported_rate')
        self.assertEqual(rc,0,folder)
        self.assert_aligned(data,encoder)
        self.assertEqual(len(data),300)
        self.assertAlmostEqual(float(data[-1]['t_s']),299/198,delta=.015)
        self.assertLess(max(float(r['source_age_ms']) for r in timing if r['err']=='0'),100)

    def test_duplicate_and_reordered_raw_are_preserved_without_refresh(self):
        folder,rc,_,control,data,encoder,timing=self.capture('duplicates')
        self.assertEqual(rc,0,folder)
        self.assert_aligned(data,encoder)
        self.assertEqual(sum(r['idx']=='-1' for r in timing),6)
        packets=[CONTROL.unpack(p) for _,p in control]
        self.assertTrue(all(b[4]>a[4] and b[5]>a[5] for a,b in zip(packets,packets[1:])))

    def test_stop_before_first_sample_does_not_publish_origin(self):
        _,rc,lines,control,data,encoder,_=self.capture('silent',stop_command='STOP')
        self.assertEqual(rc,0)
        self.assertNotIn('DATA_OK\n',lines)
        self.assertEqual(control,[])
        self.assertEqual(data,[])
        self.assertEqual(encoder,[])

    def test_pause_is_terminal_in_authoritative_stream(self):
        _,rc,_,_,data,encoder,_=self.capture('normal',duration=10,stop_command='PAUSE')
        self.assertEqual(rc,0)
        self.assertLess(len(data),100)
        self.assert_aligned(data,encoder)

    def test_buffered_stop_after_start_needs_no_extra_input(self):
        _,rc,_,_,data,encoder,_=self.capture('normal',duration=10,stop_command='BATCH')
        self.assertEqual(rc,0)
        self.assertLess(len(data),20)
        self.assert_aligned(data,encoder)

    def test_parent_pipe_close_stops_the_producer(self):
        _,rc,_,_,data,encoder,_=self.capture('normal',duration=10,stop_command='EOF')
        self.assertEqual(rc,0)
        self.assertLess(len(data),100)
        self.assert_aligned(data,encoder)


if __name__=='__main__':
    unittest.main(verbosity=2)
