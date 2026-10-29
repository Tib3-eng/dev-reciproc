"""Backend externo de curva: processo/IPC e adaptacao aos dados existentes.

Nao contem formula nem registradores do Drive. O colega altera seu gerador;
este modulo mantem o contrato com o supervisorio. Tk fica em sinewave_ui.py.
"""
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import queue
import secrets
import subprocess
import sys
import threading
import time

from sinewave_protocol import VERSION, derived, qpc_frequency, validate_request


def controller_command(repo_root, selected=''):
    if selected:
        path = Path(selected)
        if not path.is_file():
            raise FileNotFoundError('Executavel do controlador senoidal nao encontrado.')
        return [str(path)]
    candidates = [Path(sys.executable).parent / 'controladores' / 'sinewave_controller.exe',
                  Path(repo_root) / 'sinewave' / 'dist' / 'sinewave_controller.exe']
    for path in candidates:
        if path.is_file():
            return [str(path)]
    source = Path(repo_root) / 'sinewave' / 'sinewave_runtime.py'
    if not getattr(sys, 'frozen', False) and source.is_file():
        return [sys.executable, str(source)]
    raise FileNotFoundError('Instale controladores/sinewave_controller.exe junto do supervisorio.')


class ControllerClient:
    """Um leitor de stdout; stderr separado; nenhuma espera por substring READY."""
    def __init__(self, command, config_path, request):
        self.request = request
        self.messages = queue.Queue()
        self.events = []
        self.failure = ''
        self.stopped = None
        self.status = {}
        self.started = None
        self._lock = threading.Lock()
        self._write_lock = threading.Lock()
        self.stderr = open(Path(request['output']['directory']) / 'controller_stderr.log',
                           'w', encoding='ascii', errors='backslashreplace')
        self.proc = subprocess.Popen(command + ['--config', str(config_path), '--ipc'],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self.stderr, text=True, encoding='utf-8',
                                     creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        self._reader = threading.Thread(target=self._read, daemon=True)
        self._reader.start()

    def _read(self):
        try:
            for line in self.proc.stdout:
                event = json.loads(line)
                if event.get('schema_version') != VERSION or event.get('run_id') != self.request['run_id']:
                    raise ValueError('Resposta de sessao/versao incompativel')
                kind = event.get('type')
                if kind not in {'ready', 'started', 'status', 'physical_extreme', 'failed', 'stopped'}:
                    raise ValueError('Evento desconhecido do controlador')
                with self._lock:
                    self.events.append(event)
                    if kind == 'failed':
                        self.failure = self.failure or event.get('message', 'Falha no controlador')
                    elif kind == 'status':
                        self.status = event
                    elif kind == 'started':
                        self.started = event
                    elif kind == 'stopped':
                        self.stopped = event
                        if event.get('failed') or not event.get('command_confirmed'):
                            self.failure = self.failure or event.get('reason', 'Parada nao confirmada')
                self.messages.put(event)
        except Exception as exc:
            self.failure = self.failure or str(exc)
            try:
                self.send('stop', reason='protocol_error')
            except Exception:
                pass
        finally:
            self.messages.put({'type': 'eof'})

    def send(self, kind, **fields):
        if self.proc.poll() is not None:
            return
        command = {'schema_version': VERSION, 'run_id': self.request['run_id'],
                   'type': kind, 'request_id': secrets.token_hex(6), **fields}
        with self._write_lock:
            try:
                self.proc.stdin.write(json.dumps(command) + '\n')
                self.proc.stdin.flush()
            except (OSError, ValueError) as exc:
                self.failure = self.failure or 'Canal do controlador fechado: ' + str(exc)

    def wait_event(self, kind, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.failure:
                raise RuntimeError(self.failure)
            try:
                event = self.messages.get(timeout=min(.1, max(.001, deadline-time.monotonic())))
            except queue.Empty:
                continue
            if event['type'] == kind:
                return event
            if event['type'] in ('eof', 'stopped'):
                raise RuntimeError('Controlador encerrou antes de ' + kind)
        raise TimeoutError('Controlador nao respondeu ' + kind)

    def progress(self):
        target = derived(self.request)['distance_mm']
        value = max(0.0, min(target, float(self.status.get('progress_mm', 0))))
        return {'progress_mm': value, 'target_mm': target, 'remaining_mm': target-value,
                'complete': bool(self.stopped and self.stopped.get('reason') == 'cycles_complete'),
                'event': 'sine_external'}

    def close(self):
        if self.proc.poll() is None:
            self.send('stop', reason='client_close')
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                # Registrar falha: matar um processo nao equivale a parar o Drive.
                self.failure = self.failure or 'Controlador sem resposta a STOP; parada nao confirmada.'
                self.proc.kill()
                self.proc.wait(timeout=3)
        self._reader.join(timeout=2)
        if self._reader.is_alive():
            self.failure = self.failure or 'Leitor do controlador nao encerrou.'
        if self.proc.stdin:
            self.proc.stdin.close()
        if self.proc.stdout:
            self.proc.stdout.close()
        self.stderr.close()


def validate_controller(command, request, config_path):
    """Inspecao sem COM; usada ANTES de tara ou qualquer movimento preliminar."""
    Path(config_path).write_text(json.dumps(request, indent=2), encoding='ascii')
    result = subprocess.run(command + ['--config', str(config_path), '--validate'],
                            capture_output=True, text=True, encoding='utf-8', timeout=15,
                            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    if result.returncode:
        raise RuntimeError((result.stderr or result.stdout).strip())
    info = json.loads(result.stdout)
    if not info.get('valid') or info.get('schema_version') != VERSION:
        raise RuntimeError('Contrato de validacao do controlador incompativel.')
    return info


def start_run(repo_root, paths, request, selected_exe='', offset_n=0.0, cancel=None):
    """Seleciona exclusivamente o backend externo. Nunca sobe a5_speed_logger."""
    import orchestrator_runtime as orch
    d = validate_request(request)
    out = Path(request['output']['directory'])
    out.mkdir(parents=True, exist_ok=True)
    command = controller_command(repo_root, selected_exe)
    config_path = out / 'sinewave_request.json'
    if not request['simulation']:
        request['encoder_feedback'] = {'protocol': 'encoder_control_v1', 'host': '127.0.0.1',
                                       'port': orch._reserve_loopback_udp_port(),
                                       'session_id': str(secrets.randbits(63) or 1),
                                       'qpc_frequency_hz': qpc_frequency()}
    validate_controller(command, request, config_path)
    state = orch.RunState(
        dlg_proc=None, drive_proc=None, turn_proc=None,
        dlg_csv=str(out/'dlg.csv'), dlg_compat_csv=str(out/'dlg.csv'),
        encoder_state_csv=str(out/'encoder_state.csv'), drive_csv=str(out/'controller_commands.csv'),
        turn_dist_csv=paths['turn_dist_csv'], turn_vp_csv=paths['turn_vp_csv'],
        merge_csv=paths['merge_csv'], schedule_csv=str(out/'sinewave_request.json'),
        dlg_exe='', drive_exe=command[-1], turn_exe='', merge_exe='',
        duration_s=request['safety']['watchdog_s'], rate_hz=200, drive_rate_hz=50,
        force_normal_n=request['force_normal_n'], relacao=request['mechanics']['ratio'],
        raio_mm=request['mechanics']['radius_mm'], distance_interval_mm=request['distance_interval_mm'],
        reciprocating=True, reciprocating_course_mm=request['curve']['course_mm'],
        reciprocating_total_mm=d['distance_mm'], reciprocating_physical_distance_enabled=True,
        target_speed_schedule=[], dynamic_offset_n=offset_n,
        controller_kind='sine_external', controller_request=request, motion_events=[],
    )
    try:
        state.controller_client = ControllerClient(command, config_path, request)
        state.drive_proc = state.controller_client.proc
        state.controller_client.wait_event('ready')
        if not request['simulation']:
            info = orch.check_executables(repo_root)
            if not info.get('dlg_exe'):
                raise FileNotFoundError('dlg_logger_ipc.exe nao encontrado.')
            state.dlg_exe = info['dlg_exe']
            feedback = request['encoder_feedback']
            dlg_cmd = [state.dlg_exe, '--ipc', '--out', state.dlg_csv,
                       '--duration', str(state.duration_s), '--rate', '200',
                       '--ip', orch.DEFAULT_DLG_IP, '--port', str(orch.DEFAULT_DLG_PORT),
                       '--bind-port', str(orch.DEFAULT_BIND_PORT),
                       '--force-normal', str(state.force_normal_n),
                       '--encoder-state-out', state.encoder_state_csv,
                       '--encoder-radius-mm', str(state.raio_mm),
                       '--encoder-max-speed-mm-s', str(max(5., 2*d['peak_speed_mm_s'])),
                       '--encoder-target-mm', '0', '--encoder-reciprocating-shadow',
                       '--encoder-acquisition-clock', '--encoder-control-port', str(feedback['port']),
                       '--encoder-session', str(feedback['session_id'])]
            state.dlg_proc = subprocess.Popen(dlg_cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                              stderr=subprocess.STDOUT, text=True,
                                              creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            if not orch._wait_ready(state.dlg_proc, 'DLG'):
                raise RuntimeError('DLG nao respondeu READY')
            orch._send_start(state.dlg_proc)
            if not orch._wait_data_ready(state.dlg_proc, timeout_s=6):
                raise RuntimeError('DLG sem amostras validas')
            # Drenar stdout continuamente: log do DLG nao pode encher o pipe.
            def drain_dlg():
                with open(out/'dlg_stdout.log', 'w', encoding='ascii', errors='backslashreplace') as log:
                    for line in state.dlg_proc.stdout:
                        log.write(line)
            threading.Thread(target=drain_dlg, daemon=True).start()
        if cancel is not None and cancel.is_set():
            raise RuntimeError('Teste cancelado antes da partida.')
        state.controller_client.send('start')
        state.controller_client.wait_event('started', timeout=5)
        return state
    except Exception:
        if state.controller_client:
            state.controller_client.close()
        if state.dlg_proc:
            orch._send_ipc(state.dlg_proc, 'STOP')
            try:
                state.dlg_proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                state.dlg_proc.terminate()
                state.dlg_proc.wait(timeout=3)
        raise


def wait_and_finalize(state):
    """Fecha processos, preserva parciais e reutiliza o processador DLG/CH3.

    Nao existe merge por indice com o arquivo de comandos do controlador.
    Esses comandos nao sao telemetria nem amostras medidas do Drive.
    """
    import orchestrator_runtime as orch
    client = state.controller_client
    deadline = time.monotonic() + state.duration_s + 5
    try:
        while state.drive_proc.poll() is None:
            if state.dlg_proc and state.dlg_proc.poll() is not None:
                client.failure = client.failure or 'DLG encerrou durante o movimento.'
                client.send('stop', reason='dlg_exit')
            if time.monotonic() > deadline:
                client.failure = client.failure or 'Timeout do controlador externo.'
                client.send('stop', reason='supervisor_watchdog')
                break
            time.sleep(.02)
    finally:
        client.close()
        if state.dlg_proc:
            # A cauda e somente aquisicao: motor ja recebeu STOP. Nunca RUN.
            time.sleep(.5)
            orch._send_ipc(state.dlg_proc, 'STOP')
            try:
                state.dlg_proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                state.dlg_proc.terminate()
                state.dlg_proc.wait(timeout=3)
                client.failure = client.failure or 'DLG nao confirmou encerramento.'
        state.motion_events = list(client.events)
        if not client.stopped:
            client.failure = client.failure or 'Controlador encerrou sem STOPPED.'
        if state.drive_proc.returncode:
            client.failure = client.failure or 'Controlador retornou erro.'
        if state.dlg_proc and state.dlg_proc.returncode:
            client.failure = client.failure or 'Logger DLG retornou erro.'
    if os.path.isfile(state.dlg_csv) and os.path.isfile(state.encoder_state_csv):
        orch._rebuild_reciprocating_csv_from_logs(state, state.relacao, state.raio_mm,
                                                 state.distance_interval_mm)
    else:
        client.failure = client.failure or 'Arquivos DLG/CH3 ausentes.'
    return 2 if client.failure else 0


def write_info(path, state, metadata, calibration=None, final=False):
    import orchestrator_runtime as orch
    request = state.controller_request
    with open(path, 'a' if final else 'w', encoding='utf-8', newline='') as stream:
        writer = orch.InfoCsvWriter(stream)
        if not final:
            writer.writerow('campo', 'valor', 'valor2')
            for name, value in metadata.items():
                writer.writerow(name, value)
            writer.writerow('Modo', 'Senoidal (teste)')
            writer.writerow('SIMULACAO SEM HARDWARE', 'SIM' if request['simulation'] else 'NAO')
            writer.writerow('Reciprocating', 'sim')
            writer.writerow('Fonte da inversao', 'Controlador externo de velocidade multissegmento')
            writer.writerow('Fonte das medidas', 'SIMULATION_REFERENCE_V1' if request['simulation'] else 'DLG/CH3 200 Hz')
            writer.writerow('Curso pico a pico [mm]', request['curve']['course_mm'])
            writer.writerow('Frequencia [Hz]', request['curve']['frequency_hz'])
            writer.writerow('Ciclos solicitados', request['curve']['cycles'])
            writer.writerow('Raio [mm]', state.raio_mm)
            writer.writerow('Relacao mecanica (i)', state.relacao)
            writer.writerow('Forca normal [N]', state.force_normal_n)
            writer.writerow('Offset dinamico CH1 [N]', state.dynamic_offset_n)
            writer.writerow('Correcao dinamica', 'nao aplicavel: simulacao' if request['simulation'] else 'fase preliminar legada; validar aplicabilidade a senoide')
            writer.writerow('VELOCIDADE_ALVO em _M', 'NULL: referencia varia dentro do stroke')
            writer.writerow('Controlador', state.drive_exe)
            if os.path.isfile(state.drive_exe):
                writer.writerow('SHA256 controlador', hashlib.sha256(Path(state.drive_exe).read_bytes()).hexdigest())
            writer.writerow('Contrato', VERSION)
            for key, value in (calibration or {}).items():
                writer.writerow('Calibracao encoder: ' + key, value)
        else:
            writer.writerow('Motivo encerramento', (state.controller_client.stopped or {}).get('reason', 'sem STOPPED'))
            writer.writerow('Falha primaria', state.controller_client.failure or 'nenhuma')
            for key, value in (state.reciprocating_processing_summary or {}).items():
                writer.writerow('Processamento: ' + key, value if value is not None else 'NULL')
