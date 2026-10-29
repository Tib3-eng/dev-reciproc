"""Contrato v1 do controlador externo, independente de Tk e do algoritmo da curva.

Esta e a fronteira estavel entre os dois programas. Grandezas fisicas entram
em mm/Hz; somente o adaptador converte para RPM/segundos para o colega.
Nenhuma funcao deste modulo abre COM, inicia DLG ou movimenta o equipamento.
"""
import ctypes
import math
import os
import struct
import time
import uuid
import zlib

VERSION = 1
ENCODER_PACKET = struct.Struct('<IHHQQq5di4I')


def qpc():
    if os.name == 'nt':
        value = ctypes.c_longlong()
        ctypes.windll.kernel32.QueryPerformanceCounter(ctypes.byref(value))
        return value.value
    return time.perf_counter_ns()


def qpc_frequency():
    if os.name == 'nt':
        value = ctypes.c_longlong()
        ctypes.windll.kernel32.QueryPerformanceFrequency(ctypes.byref(value))
        return value.value
    return 1000000000


def positive(value, name):
    try:
        number = float(value)
    except (ValueError, TypeError):
        raise ValueError(f'{name}: informe um numero positivo.') from None
    if isinstance(value, bool) or not math.isfinite(number) or number <= 0:
        raise ValueError(f'{name}: informe um numero positivo e finito.')
    return number


def make_request(course_mm, frequency_hz, cycles, radius_mm, ratio, output_dir,
                 simulation=True):
    course = positive(course_mm, 'Curso [mm]')
    frequency = positive(frequency_hz, 'Frequencia [Hz]')
    count = positive(cycles, 'Ciclos')
    if not count.is_integer() or count > 1000000:
        raise ValueError('Ciclos: informe um inteiro entre 1 e 1000000.')
    radius = positive(radius_mm, 'Raio [mm]')
    relation = positive(ratio, 'Relacao mecanica')
    if type(simulation) is not bool:
        raise ValueError('simulation deve ser booleano.')
    duration = int(count) / frequency
    if not math.isfinite(duration) or duration > 7 * 86400:
        raise ValueError('Duracao nominal deve ser finita e de ate sete dias.')
    request = {
        'schema_version': VERSION, 'run_id': uuid.uuid4().hex,
        'simulation': simulation,
        'curve': {'profile': 'sine_position', 'course_mm': course,
                  'frequency_hz': frequency, 'cycles': int(count)},
        'mechanics': {'radius_mm': radius, 'ratio': relation},
        'serial': {'port': 'COM5', 'baud': 115200, 'parity': 'E', 'slave_id': 1},
        'output': {'directory': os.path.abspath(output_dir)},
        'safety': {'watchdog_s': max(duration * 1.5, duration + 30),
                   'source_max_age_ms': 100.0, 'course_limit_factor': 2.0},
    }
    derived(request)  # Detecta overflow antes de criar arquivos/processos.
    return request


def derived(request):
    c, m = request['curve'], request['mechanics']
    course = positive(c['course_mm'], 'course_mm')
    frequency = positive(c['frequency_hz'], 'frequency_hz')
    count = positive(c['cycles'], 'cycles')
    if not count.is_integer():
        raise ValueError('cycles deve ser inteiro.')
    radius = positive(m['radius_mm'], 'radius_mm')
    ratio = positive(m['ratio'], 'ratio')
    result = {'period_s': 1 / frequency, 'duration_s': count / frequency,
              'distance_mm': 2 * course * count,
              'peak_speed_mm_s': math.pi * course * frequency,
              'peak_motor_rpm': 30 * ratio * course * frequency / radius}
    if not all(math.isfinite(v) and v > 0 for v in result.values()):
        raise ValueError('Parametros derivados fora de faixa.')
    if result['peak_motor_rpm'] > 6000:
        raise ValueError('RPM de pico calculada excede 6000 RPM.')
    return result


def validate_request(request):
    if request.get('schema_version') != VERSION or not request.get('run_id'):
        raise ValueError('Contrato de controlador incompativel ou run_id ausente.')
    if type(request.get('simulation')) is not bool:
        raise ValueError('O pedido deve declarar simulation explicitamente.')
    if request['curve'].get('profile') != 'sine_position':
        raise ValueError('Perfil de curva nao suportado.')
    d = derived(request)
    if positive(request['safety']['watchdog_s'], 'watchdog_s') < d['duration_s']:
        raise ValueError('Watchdog menor que a duracao nominal.')
    if request['safety'].get('source_max_age_ms') != 100.0:
        raise ValueError('Preservar limite de idade de feedback de 100 ms.')
    if request['safety'].get('course_limit_factor') != 2.0:
        raise ValueError('Preservar limite fisico de 2x o curso.')
    if not os.path.isabs(request['output']['directory']):
        raise ValueError('Pasta de saida deve ser absoluta.')
    return d


def decode_encoder(data, session, last_sequence, now, frequency):
    """Espelho de encoder_control_protocol.c: 92 bytes, little-endian, CRC32.

    Heartbeats sem medida aceita nao podem renovar o prazo. A idade total
    inclui o atraso de origem informado no pacote, alem do transporte local.
    """
    if frequency <= 0:
        raise ValueError('BAD_CLOCK')
    if len(data) != 92 or ENCODER_PACKET.size != 92:
        raise ValueError('BAD_SIZE')
    values = ENCODER_PACKET.unpack(data)
    magic, version, size, received_session, seq, stamp = values[:6]
    pos, path, speed, extreme, age = values[6:11]
    direction, health, status, flags, crc = values[11:]
    if magic != 0x31434E45 or version != 1 or size != 92:
        raise ValueError('BAD_HEADER')
    if received_session != session or seq <= last_sequence:
        raise ValueError('BAD_SESSION_OR_SEQUENCE')
    if zlib.crc32(data[:88]) != crc:
        raise ValueError('BAD_CRC')
    if (stamp <= 0 or direction not in (-1, 0, 1) or health > 4 or status > 6
            or age < 0 or not all(math.isfinite(v) for v in (pos, path, speed, extreme, age))):
        raise ValueError('BAD_VALUE')
    source_age = (now - stamp) / frequency + age
    if now < stamp or source_age < 0 or source_age > .100:
        raise ValueError('STALE_OR_FUTURE')
    return {'sequence': seq, 'qpc': stamp, 'position_mm': pos, 'speed_mm_s': speed,
            'extreme_mm': extreme, 'healthy': health in (1, 2),
            'accepted': bool(flags & 1) and bool(flags & 2) and status == 0,
            'stationary': bool(flags & 4), 'reversal': bool(flags & 8),
            'source_age_s': source_age}
