"""AREA DO COLEGA: entrada pequena para sua curva, sem Tk, IPC ou arquivos DLG.

Mantivemos a formula e os 16 intervalos da copia recebida. Nao corrigimos a
curva nesta integracao. A simulacao e um modelo separado, identificado como
tal; nunca e usada como substituto silencioso do controle real.
"""
import math

GENERATOR_VERSION = 'colega-recebido-integration-v1'
# Alterar somente depois de corrigir/validar a curva e testar STOP na bancada.
# O runtime ainda valida os parametros e a tabela antes de qualquer movimento.
HARDWARE_VALIDATED = False


def plan_curve(peak_motor_rpm, period_s):
    # AREA DO COLEGA: editar aqui a forma e os quadrantes da curva.
    # Entradas: amplitude NO MOTOR [RPM] e periodo COMPLETO [s].
    # Esta expressao preserva sin(freq*t), sem 2*pi, da versao recebida.
    # O adaptador fornece a amplitude solicitada, em vez do MAX_VAL fixo.
    frequency = 1.0 / period_s
    waveform = lambda t: peak_motor_rpm * math.sin(frequency * t)
    times = [period_s / 16] * 16
    return waveform, times


def validate_hardware_plan(peak_motor_rpm, period_s):
    """AREA DE INTEGRACAO: valida sem importar Motor ou abrir COM.

    O motivo devolvido deve orientar o colega; nao habilitar por simples
    ausencia de excecao Modbus. Um readback igual nao valida a trajetoria.
    """
    waveform, times = plan_curve(peak_motor_rpm, period_s)
    t = 0.0
    speeds = []
    for dt in times:
        t += dt
        speeds.append(math.floor(waveform(t)))
    issues = []
    if not speeds or not (min(speeds) < 0 < max(speeds)):
        issues.append('A tabela recebida nao possui os dois sentidos de velocidade.')
    if any(int(dt) <= 0 or dt != int(dt) for dt in times):
        issues.append('vel_ctrl usa int(dt): revisar codificacao dos tempos fracionarios.')
    if abs(sum(v * dt for v, dt in zip(speeds, times))) > 1e-9:
        issues.append('A area assinada da tabela nao fecha em zero.')
    if not HARDWARE_VALIDATED:
        issues.append('Gerador/escala de tempo/STOP ainda nao validados para hardware.')
    if issues:
        raise ValueError(' '.join(issues))
    return {'rpm': speeds, 'input_times_s': times, 'generator_version': GENERATOR_VERSION}


def prepare_curve(motor, peak_motor_rpm, period_s):
    # AREA DE INTEGRACAO: configurar parado; NAO chamar start nesta funcao.
    table = validate_hardware_plan(peak_motor_rpm, period_s)
    waveform, times = plan_curve(peak_motor_rpm, period_s)
    motor.vel_ctrl(waveform, times)
    return table


def start_curve(motor):
    # Retorno somente depois de o comando ser aceito. Erros propagam.
    motor.start()


def stop_curve(motor):
    # Deve ser idempotente e funcionar mesmo depois de setup parcial.
    motor.stop()
