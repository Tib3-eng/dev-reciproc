"""AREA DE INTEGRACAO (nossa): processo externo v1, sem dependencias de Tk.

A curva do colega esta em curve_program.py. Este arquivo cuida de protocolo,
arquivos, simulacao explicita e encerramento. STDOUT e reservado a JSONL;
prints do Motor ficam em STDERR. Nao existe partida automatica ao abrir COM.
"""
import argparse
import contextlib
import csv
import json
import math
import queue
import socket
import sys
import threading
import time
from pathlib import Path

if not getattr(sys, 'frozen', False):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'Supervis\u00f3rio'))
from sinewave_protocol import VERSION, decode_encoder, derived, qpc, qpc_frequency, validate_request
import curve_program as engine

BUILD = 'sinewave-integration-1'


def describe():
    return {'schema_version': VERSION, 'controller_version': BUILD,
            'generator_version': engine.GENERATOR_VERSION,
            'capabilities': {'simulation': True, 'hardware_validated': engine.HARDWARE_VALIDATED,
                             'pause_resume': False}}


def run(request, incoming=None):
    d = validate_request(request)
    out = Path(request['output']['directory'])
    out.mkdir(parents=True, exist_ok=True)
    commands = incoming or queue.Queue()
    events = (out / 'controller_events.jsonl').open('w', encoding='ascii', buffering=1)
    motor = None
    sock = None
    failed = False
    reason = 'cancelled_before_start'
    last_command = None

    def emit(kind, **fields):
        event = {'schema_version': VERSION, 'run_id': request['run_id'],
                 'type': kind, 'qpc': qpc(), **fields}
        line = json.dumps(event, ensure_ascii=True, allow_nan=False)
        events.write(line + '\n')
        print(line, flush=True)

    def read_commands():
        # Um unico leitor; EOF e uma solicitacao de parada, nunca de RUN.
        try:
            for line in sys.stdin:
                try:
                    event = json.loads(line)
                    if event.get('schema_version') != VERSION or event.get('run_id') != request['run_id']:
                        raise ValueError('Mensagem de outra sessao/versao')
                    commands.put(event)
                except Exception as exc:
                    commands.put({'type': 'protocol_error', 'message': str(exc)})
        finally:
            commands.put({'type': 'stop', 'reason': 'stdin_eof'})

    if incoming is None:
        threading.Thread(target=read_commands, daemon=True).start()

    def poll_command():
        nonlocal last_command
        try:
            last_command = commands.get_nowait()
            return last_command
        except queue.Empty:
            return None

    try:
        if request['simulation']:
            effective = {'model': 'SIMULATION_REFERENCE_V1', **d}
        else:
            # Validar o gerador recebido ANTES de importar Motor/abrir COM.
            effective = engine.validate_hardware_plan(d['peak_motor_rpm'], d['period_s'])
            feedback = request['encoder_feedback']
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.bind(('127.0.0.1', int(feedback['port'])))
            sock.setblocking(False)
            from motor import Motor
            serial = request['serial']
            with contextlib.redirect_stdout(sys.stderr):
                motor = Motor(serial['port'], serial['baud'], serial['parity'], serial['slave_id'])
                motor.stop()
                effective = engine.prepare_curve(motor, d['peak_motor_rpm'], d['period_s'])
                for addr, value in ((0x0200, 0), (0x0C09, 1), (0x0302, 0), (0x3100, 0)):
                    if motor._read(addr) != value:
                        raise RuntimeError(f'Setup divergiu em 0x{addr:04X}')
        (out / 'sinewave_effective.json').write_text(json.dumps(effective, indent=2), encoding='ascii')
        emit('ready', **describe(), effective=effective)
        startup_deadline = time.monotonic() + 30
        while True:
            command = poll_command()
            if command:
                if command['type'] == 'start':
                    break
                if command['type'] in ('stop', 'shutdown'):
                    reason = command.get('reason', 'cancelled_before_start')
                    return 0
                raise RuntimeError('Comando invalido antes da partida')
            if time.monotonic() > startup_deadline:
                raise RuntimeError('Timeout aguardando START')
            time.sleep(.005)

        if motor:
            # DATA_OK do DLG nao substitui a origem UDP causal. A barreira
            # abaixo mantem o Drive parado ate aceitar uma medida saudavel.
            latest = None
            sequence = 0
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline and latest is None:
                command = poll_command()
                if command:
                    if command['type'] == 'stop':
                        reason = command.get('reason', 'operator')
                        return 0
                    raise RuntimeError('Comando invalido na barreira de origem')
                try:
                    data, peer = sock.recvfrom(256)
                    if peer[0] != '127.0.0.1':
                        continue
                    sample = decode_encoder(data, int(feedback['session_id']), sequence,
                                            qpc(), qpc_frequency())
                    sequence = sample['sequence']
                    if sample['accepted'] and sample['healthy']:
                        latest = sample
                except (BlockingIOError, ValueError):
                    time.sleep(.002)
            if latest is None:
                raise RuntimeError('RECIP_ENCODER_NO_ORIGIN')
            origin = latest['position_mm']
            last_accepted_at = time.monotonic() - latest['source_age_s']
            with contextlib.redirect_stdout(sys.stderr):
                engine.start_curve(motor)

        started = time.monotonic()
        started_qpc = qpc()
        emit('started', request_id=last_command.get('request_id'), command_confirmed=True)
        if request['simulation']:
            reason = simulate(request, d, started, started_qpc, emit, poll_command)
        else:
            reversals = 0
            completed_distance = 0.0
            stroke_origin = origin
            local_progress = 0.0
            last_status = 0.0
            moved = False
            while True:
                command = poll_command()
                if command:
                    if command['type'] == 'stop':
                        reason = command.get('reason', 'operator')
                        break
                    raise RuntimeError('Comando nao suportado durante movimento')
                elapsed = time.monotonic() - started
                if elapsed > request['safety']['watchdog_s']:
                    raise RuntimeError('WATCHDOG_EXCEEDED')
                # Consumir todos os pacotes disponiveis, sem atrelar feedback
                # ao refresh Tk ou ao relogio de gravacao do Drive.
                for _ in range(512):
                    try:
                        data, peer = sock.recvfrom(256)
                    except BlockingIOError:
                        break
                    if peer[0] != '127.0.0.1':
                        continue
                    try:
                        sample = decode_encoder(data, int(feedback['session_id']), sequence,
                                                qpc(), qpc_frequency())
                    except ValueError:
                        continue
                    sequence = sample['sequence']
                    if not sample['healthy']:
                        raise RuntimeError('ENCODER_UNHEALTHY')
                    if sample['accepted']:
                        latest = sample
                        last_accepted_at = time.monotonic() - sample['source_age_s']
                        displacement = abs(sample['position_mm'] - origin)
                        moved = moved or displacement >= .2
                        if displacement >= 2 * request['curve']['course_mm']:
                            raise RuntimeError('POSITION_LIMIT_2X_COURSE')
                        if sample['reversal']:
                            reversals += 1
                            completed_distance += abs(sample['extreme_mm'] - stroke_origin)
                            stroke_origin = sample['extreme_mm']
                            local_progress = 0.0
                            emit('physical_extreme', stroke=reversals, sample_qpc=sample['qpc'],
                                 position_mm=sample['extreme_mm'], source='encoder')
                        local_progress = max(local_progress, abs(sample['position_mm'] - stroke_origin))
                if time.monotonic() - last_accepted_at > .100:
                    raise RuntimeError('ENCODER_SOURCE_STALE')
                if not moved and elapsed > 3:
                    raise RuntimeError('RECIP_ENCODER_NO_MOTION')
                if (elapsed >= d['duration_s'] and
                        reversals >= 2 * request['curve']['cycles'] - 1 and
                        abs(latest['position_mm'] - origin) <= .2 and
                        abs(latest['speed_mm_s']) <= .25):
                    reason = 'cycles_complete'
                    break
                if elapsed - last_status >= .5:
                    emit('status', elapsed_s=elapsed, measured_cycles=reversals // 2,
                         progress_mm=completed_distance + local_progress)
                    last_status = elapsed
                time.sleep(.002)
        return 0
    except Exception as exc:
        failed = True
        reason = str(exc)
        emit('failed', code='CONTROLLER_FAILED', message=str(exc))
        return 2
    finally:
        confirmed = motor is None
        try:
            if motor:
                with contextlib.redirect_stdout(sys.stderr):
                    engine.stop_curve(motor)
                confirmed = True
        except Exception as exc:
            emit('failed', code='STOP_NOT_CONFIRMED', message=str(exc))
            confirmed = False
        finally:
            if motor:
                motor.close()
            if sock:
                sock.close()
        emit('stopped', reason=reason, command_confirmed=confirmed,
             feedback_stationary=True if request['simulation'] else None,
             failed=failed or not confirmed,
             request_id=(last_command or {}).get('request_id'))
        events.close()
        if not confirmed:
            return 2


def simulate(request, d, started, started_qpc, emit, poll_command):
    """Modelo de integracao, NAO simulacao do algoritmo recebido do colega.

    Gera DLG/CH3 sinteticos a 200 Hz, com idx/QPC identicos, em tempo real.
    Os arquivos e a UI identificam o modo SIMULACAO. Nao importa pymodbus.
    """
    out = Path(request['output']['directory'])
    course, f = request['curve']['course_mm'], request['curve']['frequency_hz']
    radius = request['mechanics']['radius_mm']
    force = request.get('force_normal_n', 1.0)
    count = int(round(d['duration_s'] * 200)) + 1
    previous_stroke = 0
    with (out / 'dlg.csv').open('w', encoding='ascii', newline='', buffering=1) as fd, \
         (out / 'encoder_state.csv').open('w', encoding='ascii', newline='', buffering=1) as fe:
        dw, ew = csv.writer(fd), csv.writer(fe)
        dw.writerow(['idx','t_qpc','t_s',*[f'ch{i}' for i in range(1,9)],'atrito','err'])
        ew.writerow('idx,t_qpc,t_s,angle_deg,accepted,status,health,unwrapped_deg,relative_deg,relative_mm,progress_mm,speed_mm_s,disk_rpm,direction,stationary,reversal,extreme_relative_mm,accepted_age_s'.split(','))
        for idx in range(count):
            t = idx / 200
            while time.monotonic() - started < t:
                command = poll_command()
                if command:
                    if command['type'] == 'stop':
                        return command.get('reason', 'operator')
                    raise RuntimeError('Comando invalido na simulacao')
                time.sleep(.002)
            command = poll_command()
            if command:
                if command['type'] == 'stop':
                    return command.get('reason', 'operator')
                raise RuntimeError('Comando invalido na simulacao')
            phase = 2 * math.pi * f * t
            pos = course * .5 * (1 - math.cos(phase))
            speed = math.pi * course * f * math.sin(phase)
            angle = math.degrees(pos / radius)
            stamp = min(qpc(), started_qpc + round(t * qpc_frequency()))
            cof = .15 * math.tanh(speed * 10) + .01 * math.sin(phase * 3)
            stroke = min(int((t + 1e-9) * 2 * f), 2 * request['curve']['cycles'])
            reversal = int(stroke > previous_stroke)
            dw.writerow([idx,stamp,f'{t:.6f}',cof*force,25+.02*t,angle%360,0,0,0,0,0,cof,0])
            ew.writerow([idx,stamp,f'{t:.6f}',angle%360,1,0,1,angle,angle,pos,
                         stroke*course+abs(pos-(course if stroke%2 else 0)),speed,
                         speed*60/(2*math.pi*radius),1 if speed>=0 else -1,
                         int(abs(speed)<.001),reversal,pos,0])
            if reversal:
                emit('physical_extreme', stroke=stroke, idx=idx, sample_qpc=stamp,
                     position_mm=pos, source='simulated_encoder')
                previous_stroke = stroke
            if idx % 100 == 0:
                emit('status', elapsed_s=t, measured_cycles=stroke//2,
                     progress_mm=stroke*course, simulation=True)
    return 'cycles_complete'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config')
    parser.add_argument('--ipc', action='store_true')
    parser.add_argument('--describe', action='store_true')
    parser.add_argument('--validate', action='store_true')
    args = parser.parse_args()
    if args.describe:
        print(json.dumps(describe()), flush=True)
        return 0
    if not args.config:
        parser.error('--config e obrigatorio')
    request = json.loads(Path(args.config).read_text(encoding='utf-8'))
    if args.validate:
        d = validate_request(request)
        if not request['simulation']:
            engine.validate_hardware_plan(d['peak_motor_rpm'], d['period_s'])
        print(json.dumps({'valid': True, **describe()}))
        return 0
    if not args.ipc:
        parser.error('--ipc e obrigatorio; nao existe partida automatica')
    return run(request)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        sys.exit(2)
