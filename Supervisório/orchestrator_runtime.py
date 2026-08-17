"""
orchestrator_runtime.py
-----------------------
Orquestrador de execucao usado pelo novo_tribometro.py.

Objetivo geral:
- Concentrar neste modulo toda a logica de runtime do ensaio (processos,
  arquivos e sincronizacao), para manter a interface grafica mais limpa.

Objetivos praticos:
- Iniciar e parar os executaveis C em modo sem interface grafica (DLG + Drive).
- Controlar os processos por IPC de linha unica (stdin/stdout), com comandos
  como START, PAUSE, RESUME e STOP.
- Gerar estrutura de saida previsivel (pasta + CSVs) para facilitar auditoria.
- Rodar merge final dos logs e entregar um CSV consolidado ao supervisorio.
- Separar artefatos tecnicos em subpasta DadosDev para nao poluir a pasta de execucao.

Escopo:
- Resolver caminhos de executaveis.
- Criar e validar caminhos de saida.
- Ligar/desligar subprocessos externos com parametros explicitos.
- Aplicar alternativa de merge em Python quando o merge em C falhar.

Fora de escopo:
- Nao desenha UI.
- Nao atualiza widgets Tkinter.
- Nao implementa calibracao nem controle detalhado de hardware.
"""

import csv
import bisect
import math
import os
import secrets
import shutil
import socket
import subprocess
import sys
import threading
import time
from itertools import zip_longest
from dataclasses import dataclass
from pathlib import Path
from typing import List, Tuple, Optional


# -------------------------------
# Configuracoes padrao (podem ser sobrescritas por quem chama o modulo)
# -------------------------------
DEFAULT_DLG_RATE_HZ = 200.0
DEFAULT_DRIVE_RATE_HZ = 50.0
# Nome legado mantido para chamadores que representam a taxa do DLG.
DEFAULT_RATE_HZ = DEFAULT_DLG_RATE_HZ
DEFAULT_DLG_IP = "192.168.1.100"
DEFAULT_DLG_PORT = 41401
DEFAULT_BIND_IP = ""
DEFAULT_BIND_PORT = 41402

# Filtro operacional do encoder externo. O CH3 ja chega convertido para graus;
# estes limites atuam somente sobre transicoes fisicamente incompativeis.
ENCODER_TRANSITION_MIN_GATE_DEG = 3.0
ENCODER_TRANSITION_CONFIRM_S = 0.100
ENCODER_TRANSITION_VELOCITY_HISTORY_S = 0.300

# Modelo validado no modo sombra e agora usado pelo controle reciprocante
# baseado exclusivamente no encoder externo do DLG.
RECIP_SHADOW_STOP_MODEL_SLOPE_S = 0.13199835832212273
RECIP_SHADOW_STOP_MARGIN_MM = 0.27025001630016243
RECIP_SHADOW_STOP_VELOCITY_WINDOW_S = 0.250
RECIP_SHADOW_STOP_MAX_COURSE_FRACTION = 0.45

# Confirmacao do ultimo extremo reciprocante apos o comando de parada. Nao ha
# reversao no ultimo stroke, portanto a conclusao usa movimento residual seguido
# de estabilidade do proprio CH3. Os limites estao em milimetros no disco.
RECIP_FINAL_MEDIAN_SAMPLES = 9
RECIP_FINAL_MIN_OBSERVATION_S = 0.450
RECIP_FINAL_STABLE_WINDOW_S = 0.250
RECIP_FINAL_STABLE_MIN_SPAN_S = 0.225
RECIP_FINAL_STABLE_RANGE_MM = 0.150
RECIP_FINAL_MAX_SPEED_MM_S = 0.250
RECIP_FINAL_MIN_EXCURSION_MM = 0.200

# Trava causal de sentido do modo continuo. A mediana curta elimina a oscilacao
# eletrica das transicoes; a decisao usa deslocamento liquido desde a origem.
# Estes parametros nao participam do controle reciprocante.
CONT_DIRECTION_MEDIAN_SAMPLES = 51
CONT_DIRECTION_MOTION_MM = 0.2
CONT_DIRECTION_LOCK_MM = 1.0
CONT_DIRECTION_CONFIRM_SAMPLES = 5
CONT_DIRECTION_LOCK_TIMEOUT_S = 3.0
CONT_DIRECTION_INCONSISTENT_MM = 1.0


class _ContinuousDirectionTracker:
    """Reconstrucao Python equivalente a trava causal do logger C."""

    def __init__(self):
        self._recent = []
        self.direction = 0
        self.candidate_direction = 0
        self.candidate_samples = 0
        self.filtered_relative_mm = 0.0
        self.movement_t_s = None
        self.lock_t_s = None
        self.max_progress_mm = 0.0
        self.fault = None

    def update(self, t_s, relative_mm, accepted):
        if self.fault is not None:
            return self.max_progress_mm if self.direction else 0.0
        if accepted and relative_mm is not None:
            self._recent.append(float(relative_mm))
            if len(self._recent) > CONT_DIRECTION_MEDIAN_SAMPLES:
                del self._recent[0]
            ordered = sorted(self._recent)
            self.filtered_relative_mm = ordered[len(ordered) // 2]
            if (
                self.movement_t_s is None and
                abs(self.filtered_relative_mm) >= CONT_DIRECTION_MOTION_MM
            ):
                self.movement_t_s = float(t_s)
            if self.direction == 0:
                if abs(self.filtered_relative_mm) >= CONT_DIRECTION_LOCK_MM:
                    sign = 1 if self.filtered_relative_mm >= 0.0 else -1
                    if self.candidate_direction == sign:
                        self.candidate_samples += 1
                    else:
                        self.candidate_direction = sign
                        self.candidate_samples = 1
                    if self.candidate_samples >= CONT_DIRECTION_CONFIRM_SAMPLES:
                        self.direction = sign
                        self.lock_t_s = float(t_s)
                else:
                    self.candidate_direction = 0
                    self.candidate_samples = 0
            if (
                self.direction != 0 and
                self.direction * self.filtered_relative_mm <= -CONT_DIRECTION_INCONSISTENT_MM
            ):
                self.fault = "DIRECTION_INCONSISTENT"
            if self.direction != 0:
                directional_mm = self.direction * float(relative_mm)
                self.max_progress_mm = max(self.max_progress_mm, directional_mm, 0.0)
        if (
            self.direction == 0 and self.movement_t_s is not None and
            float(t_s) - self.movement_t_s >= CONT_DIRECTION_LOCK_TIMEOUT_S
        ):
            self.fault = "DIRECTION_LOCK_TIMEOUT"
        return self.max_progress_mm if self.direction else 0.0


class InfoCsvWriter:
    """Escreve as tres colunas do _I com separador ';'.

    ``write`` aceita as linhas internas historicas ``campo,valor,valor2`` para
    manter a montagem dos metadados centralizada. A primeira e a ultima virgula
    delimitam as colunas; virgulas dentro do valor central sao preservadas.
    """

    def __init__(self, stream):
        self._writer = csv.writer(stream, delimiter=";", lineterminator="\n")

    def writerow(self, field, value="", value2=""):
        self._writer.writerow([field, value, value2])

    def write(self, text):
        for line in str(text).splitlines():
            first = line.find(",")
            last = line.rfind(",")
            if first < 0:
                self.writerow(line)
            elif first == last:
                self.writerow(line[:first], line[first + 1:])
            else:
                self.writerow(
                    line[:first],
                    line[first + 1:last],
                    line[last + 1:],
                )
        return len(str(text))


@dataclass
class RunState:
    # Handles dos subprocessos em execucao.
    dlg_proc: subprocess.Popen
    drive_proc: subprocess.Popen
    turn_proc: Optional[subprocess.Popen]
    # Caminhos de arquivos de saida do ensaio.
    dlg_csv: str
    dlg_compat_csv: str
    encoder_state_csv: str
    drive_csv: str
    turn_dist_csv: str
    turn_vp_csv: str
    merge_csv: str
    schedule_csv: str
    # Caminhos dos executaveis usados (util para diagnostico em log).
    dlg_exe: str
    drive_exe: str
    turn_exe: str
    merge_exe: str
    # Parametros de execucao aplicados neste ensaio.
    duration_s: float
    rate_hz: float
    drive_rate_hz: float
    force_normal_n: float
    relacao: float
    raio_mm: float = 0.0
    distance_interval_mm: float = 10.0
    reciprocating: bool = False
    reciprocating_course_mm: float = 0.0
    reciprocating_total_mm: float = 0.0
    reciprocating_tolerance_counts: int = 0
    reciprocating_edge_filter_pct: float = 0.0
    reciprocating_shadow_enabled: bool = False
    reciprocating_encoder_control_enabled: bool = False
    drive_command_only: bool = False
    reciprocating_shadow_port: int = 0
    reciprocating_shadow_session: int = 0
    reciprocating_shadow_tolerance_mm: float = 0.0
    reciprocating_shadow_total_mm: float = 0.0
    reciprocating_shadow_stroke_timeout_s: float = 0.0
    reciprocating_shadow_forward_sign: int = 0
    reciprocating_shadow_stop_compensation: bool = False
    reciprocating_shadow_stop_slope_s: float = 0.0
    reciprocating_shadow_stop_margin_mm: float = 0.0
    reciprocating_shadow_stop_velocity_window_s: float = 0.0
    reciprocating_shadow_stop_max_course_fraction: float = 0.0
    reciprocating_shadow_summary: Optional[dict] = None
    reciprocating_stop_diagnostics: Optional[dict] = None
    reciprocating_processing_summary: Optional[dict] = None
    target_speed_schedule: Optional[List[Tuple[float, float]]] = None
    continuous_target_mm: float = 0.0
    continuous_encoder_status: str = "nao_aplicavel"
    continuous_encoder_target_t_s: Optional[float] = None
    continuous_encoder_message: str = ""
    encoder_monitor_thread: Optional[threading.Thread] = None
    manual_stop_requested: bool = False
    reciprocating_final_encoder_idx: Optional[int] = None
    reciprocating_final_encoder_t_s: Optional[float] = None
    reciprocating_postroll_status: str = "nao_aplicavel"
    encoder_quarantine_samples: int = 0
    encoder_quarantine_fraction: float = 0.0
    # Correcao aditiva de CH1 calculada na fase preliminar reciprocante.
    dynamic_offset_n: float = 0.0
    # Estado logico de pausa observado pelo orquestrador.
    paused: bool = False


def _processing_dlg_csv(state: RunState) -> str:
    """DLG alinhado por indice com o Drive para o pipeline legado."""
    compat = getattr(state, "dlg_compat_csv", "") or ""
    return compat if compat else state.dlg_csv


def _processing_rate_hz(state: RunState) -> float:
    """Taxa comum usada no merge e nos processamentos por indice."""
    rate = getattr(state, "drive_rate_hz", 0.0) or 0.0
    if rate > 0.0:
        return float(rate)
    return float(getattr(state, "rate_hz", DEFAULT_DRIVE_RATE_HZ) or DEFAULT_DRIVE_RATE_HZ)


def _event_fields(line: str) -> dict:
    fields = {}
    for token in str(line).replace(",", " ").split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key.strip()] = value.strip()
    return fields


def _detect_reciprocating_stationary_endpoint(
    post_angles: List[Tuple[int, float, float]],
    radius_mm: float,
) -> Optional[Tuple[int, float, float]]:
    """Detecta o extremo final quando o ultimo stroke termina sem reversao.

    As amostras chegam como angulo ja desenrolado. Uma mediana causal curta
    remove as oscilacoes de transicao; a decisao exige excursao depois do
    gatilho e uma janela sustentada de estabilidade junto ao extremo atingido.
    """
    if len(post_angles) < RECIP_FINAL_MEDIAN_SAMPLES or radius_mm <= 0.0:
        return None
    mm_per_degree = (2.0 * math.pi * float(radius_mm)) / 360.0
    filtered = []
    window = []
    for idx, t_s, angle_deg in post_angles:
        window.append(float(angle_deg))
        if len(window) > RECIP_FINAL_MEDIAN_SAMPLES:
            del window[0]
        ordered = sorted(window)
        median_angle = ordered[len(ordered) // 2]
        filtered.append((int(idx), float(t_s), median_angle * mm_per_degree))

    # Procura a primeira confirmacao causal. Assim um replay com toda a cauda
    # retorna o mesmo extremo que teria sido escolhido durante a aquisicao, sem
    # desloca-lo para um minimo/maximo posterior produzido por jitter parado.
    for end_pos, current in enumerate(filtered):
        if current[1] - filtered[0][1] < RECIP_FINAL_MIN_OBSERVATION_S:
            continue
        prefix = filtered[:end_pos + 1]
        values = [item[2] for item in prefix]
        upward_excursion = max(values) - values[0]
        downward_excursion = values[0] - min(values)
        direction = 1 if upward_excursion >= downward_excursion else -1
        if max(upward_excursion, downward_excursion) < RECIP_FINAL_MIN_EXCURSION_MM:
            continue

        stable_start = current[1] - RECIP_FINAL_STABLE_WINDOW_S
        recent = [item for item in prefix if item[1] >= stable_start]
        if len(recent) < 2:
            continue
        if recent[-1][1] - recent[0][1] < RECIP_FINAL_STABLE_MIN_SPAN_S:
            continue
        recent_values = [item[2] for item in recent]
        if max(recent_values) - min(recent_values) > RECIP_FINAL_STABLE_RANGE_MM:
            continue
        mean_t = sum(item[1] for item in recent) / len(recent)
        mean_value = sum(recent_values) / len(recent_values)
        denominator = sum((item[1] - mean_t) ** 2 for item in recent)
        slope_mm_s = (
            sum(
                (item[1] - mean_t) * (item[2] - mean_value)
                for item in recent
            ) / denominator
            if denominator > 0.0 else 0.0
        )
        if abs(slope_mm_s) > RECIP_FINAL_MAX_SPEED_MM_S:
            continue

        endpoint = (
            max(prefix, key=lambda item: item[2])
            if direction > 0 else
            min(prefix, key=lambda item: item[2])
        )
        recent_center = sorted(recent_values)[len(recent_values) // 2]
        if abs(recent_center - endpoint[2]) <= RECIP_FINAL_STABLE_RANGE_MM:
            return endpoint
    return None


def _reciprocating_startup_failure_reason(event_path: str) -> Optional[str]:
    """Traduz uma falha primaria anterior ao movimento sem listar efeitos em cascata."""
    if not event_path or not os.path.isfile(event_path):
        return None
    try:
        with open(event_path, "r", encoding="ascii", errors="replace") as stream:
            for line in stream:
                if "FATAL RECIP_ENCODER_NO_ORIGIN" in line:
                    fields = _event_fields(line)
                    timeout_s = fields.get("timeout_s", "3.000")
                    try:
                        timeout_text = f"{float(timeout_s):g}"
                    except Exception:
                        timeout_text = "3"
                    return (
                        "Encoder externo não entregou origem válida em "
                        f"{timeout_text} s; motor não foi ligado"
                    )
    except Exception:
        return None
    return None


def _reserve_loopback_udp_port() -> int:
    """Escolhe uma porta UDP local; a sessao aleatoria rejeita datagramas antigos."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def load_reciprocating_shadow_summary(state: RunState) -> dict:
    """Le o resumo final do receptor sombra sem alterar o resultado do ensaio."""
    if not state or not (
        getattr(state, "reciprocating_shadow_enabled", False) or
        getattr(state, "reciprocating_encoder_control_enabled", False)
    ):
        return {}
    rep_dir = os.path.dirname(getattr(state, "merge_csv", "") or "")
    candidates = [
        os.path.join(rep_dir, "a5_speed_events.log"),
        os.path.join(rep_dir, "DadosDev", "a5_speed_events.log"),
    ]
    summary = {}
    legacy_trigger_indices = []
    shadow_reverse_indices = []
    legacy_done_idx = None
    shadow_complete_idx = None
    for path in candidates:
        if not os.path.isfile(path):
            continue
        try:
            with open(path, "r", encoding="ascii", errors="replace") as stream:
                for line in stream:
                    if "RECIP_SHADOW_END " in line:
                        summary = _event_fields(line)
                    elif "RECIP_STOP_TRIGGER " in line:
                        fields = _event_fields(line)
                        try:
                            legacy_trigger_indices.append(int(fields["idx"]))
                        except Exception:
                            pass
                    elif "RECIP_SHADOW_REVERSE " in line:
                        fields = _event_fields(line)
                        try:
                            shadow_reverse_indices.append(int(fields["idx"]))
                        except Exception:
                            pass
                    elif "RECIP_DONE " in line:
                        fields = _event_fields(line)
                        try:
                            legacy_done_idx = int(fields["idx"])
                        except Exception:
                            pass
                    elif "RECIP_SHADOW_COMPLETE " in line:
                        fields = _event_fields(line)
                        try:
                            shadow_complete_idx = int(fields["idx"])
                        except Exception:
                            pass
        except Exception:
            continue
        if summary:
            break
    pair_count = min(len(legacy_trigger_indices), len(shadow_reverse_indices))
    rate_hz = float(getattr(state, "drive_rate_hz", DEFAULT_DRIVE_RATE_HZ) or DEFAULT_DRIVE_RATE_HZ)
    reverse_delta_ms = [
        1000.0 * (shadow_reverse_indices[i] - legacy_trigger_indices[i]) / rate_hz
        for i in range(pair_count)
    ]
    summary.update({
        "legacy_stop_triggers": str(len(legacy_trigger_indices)),
        "shadow_reverse_events_parsed": str(len(shadow_reverse_indices)),
        "comparison_pairs": str(pair_count),
        "reverse_delta_mean_ms": (
            f"{sum(reverse_delta_ms) / pair_count:.6f}" if pair_count else "NULL"
        ),
        "reverse_delta_min_ms": (
            f"{min(reverse_delta_ms):.6f}" if pair_count else "NULL"
        ),
        "reverse_delta_max_ms": (
            f"{max(reverse_delta_ms):.6f}" if pair_count else "NULL"
        ),
        "completion_delta_ms": (
            f"{1000.0 * (shadow_complete_idx - legacy_done_idx) / rate_hz:.6f}"
            if legacy_done_idx is not None and shadow_complete_idx is not None
            else "NULL"
        ),
    })
    state.reciprocating_shadow_summary = summary
    return summary


def analyze_encoder_state_capture(path: str, expected_path_mm: float) -> dict:
    """Extrai qualidade observada do CH3; nao participa das decisoes de controle."""
    positions = []
    accepted = 0
    missing = 0
    if not path or not os.path.isfile(path):
        return {}
    try:
        with open(path, "r", newline="", encoding="ascii", errors="replace") as stream:
            for row in csv.DictReader(stream):
                if str(row.get("accepted", "0")) == "1":
                    try:
                        positions.append(float(row["relative_mm"]))
                        accepted += 1
                    except Exception:
                        missing += 1
                else:
                    missing += 1
    except Exception:
        return {}
    increments = [abs(current - previous) for previous, current in zip(positions, positions[1:])]
    ordered = sorted(increments)
    p95 = ordered[min(len(ordered) - 1, int(0.95 * len(ordered)))] if ordered else 0.0
    total_variation = sum(increments)
    return {
        "accepted_samples": accepted,
        "missing_samples": missing,
        "relative_span_mm": (max(positions) - min(positions)) if positions else 0.0,
        "increment_abs_median_mm": _median(increments) if increments else 0.0,
        "increment_abs_p95_mm": p95,
        "raw_total_variation_mm": total_variation,
        "raw_variation_ratio": (
            total_variation / float(expected_path_mm)
            if expected_path_mm > 0.0 else 0.0
        ),
    }


def build_reciprocating_stop_diagnostics(
    state: RunState,
    output_path: Optional[str] = None,
    phase: str = "ensaio_oficial",
    velocity_window_s: float = 0.250,
    event_path: Optional[str] = None,
) -> dict:
    """Consolida, por stroke, dados para estudar a distancia de parada.

    O arquivo e exclusivamente diagnostico. A velocidade e a inclinacao
    linear de PosEncExt nos ``velocity_window_s`` anteriores ao gatilho que
    realmente comandou a parada (externo no pipeline atual).
    """
    if not state or not (
        getattr(state, "reciprocating_shadow_enabled", False) or
        getattr(state, "reciprocating_encoder_control_enabled", False)
    ):
        return {}
    rep_dir = os.path.dirname(getattr(state, "merge_csv", "") or "")
    if not rep_dir:
        return {}

    def _existing(*paths):
        for path in paths:
            if path and os.path.isfile(path):
                return path
        return ""

    event_path = _existing(
        event_path,
        os.path.join(os.path.dirname(getattr(state, "drive_csv", "") or ""), "a5_speed_events.log"),
        os.path.join(rep_dir, "a5_speed_events.log"),
        os.path.join(rep_dir, "DadosDev", "a5_speed_events.log"),
    )
    drive_path = _existing(
        getattr(state, "drive_csv", ""),
        os.path.join(rep_dir, "DadosDev", "drive.csv"),
    )
    if getattr(state, "reciprocating_encoder_control_enabled", False):
        drive_path = ""
    encoder_path = _existing(
        getattr(state, "encoder_state_csv", ""),
        os.path.join(rep_dir, "DadosDev", "encoder_state.csv"),
    )
    if not event_path or not encoder_path:
        return {}

    def _number(value, cast=float, default=None):
        try:
            if value is None or str(value).strip().upper() in ("", "NULL"):
                return default
            return cast(value)
        except Exception:
            return default

    events = {
        "legacy": [], "completion": [], "external": [], "physical": [],
    }
    try:
        with open(event_path, "r", encoding="ascii", errors="replace") as stream:
            for line in stream:
                fields = _event_fields(line)
                if "RECIP_STOP_TRIGGER " in line:
                    events["legacy"].append(fields)
                elif "RECIP_REVERSE " in line or "RECIP_DONE " in line:
                    events["completion"].append(fields)
                elif "RECIP_SHADOW_REVERSE " in line or "RECIP_SHADOW_COMPLETE " in line:
                    events["external"].append(fields)
                elif "RECIP_SHADOW_PHYSICAL_REVERSAL " in line:
                    events["physical"].append(fields)
    except Exception:
        return {}
    for values in events.values():
        values.sort(key=lambda item: _number(item.get("idx"), int, 2**31 - 1))

    drive_qpc = {}
    if drive_path:
        try:
            with open(drive_path, "r", newline="", encoding="ascii", errors="replace") as stream:
                for row in csv.DictReader(stream):
                    idx = _number(row.get("idx"), int)
                    qpc = _number(row.get("t_qpc"), int)
                    if idx is not None and qpc is not None:
                        drive_qpc[idx] = qpc
        except Exception:
            drive_qpc = {}

    samples = []
    try:
        with open(encoder_path, "r", newline="", encoding="ascii", errors="replace") as stream:
            for row in csv.DictReader(stream):
                if str(row.get("accepted", "0")).strip() != "1":
                    continue
                qpc = _number(row.get("t_qpc"), int)
                t_s = _number(row.get("t_s"), float)
                position = _number(row.get("relative_mm"), float)
                if qpc is not None and t_s is not None and position is not None:
                    samples.append((qpc, t_s, position))
    except Exception:
        return {}
    samples.sort(key=lambda item: item[0])
    if not samples:
        return {}
    qpcs = [item[0] for item in samples]
    qpc_frequency = 0.0
    if len(samples) >= 2 and samples[-1][1] > samples[0][1]:
        qpc_frequency = (samples[-1][0] - samples[0][0]) / (samples[-1][1] - samples[0][1])
    if qpc_frequency <= 0.0:
        return {}
    qpc_origin = samples[0][0]

    def _interpolated_position(qpc):
        if qpc is None:
            return None
        pos = bisect.bisect_left(qpcs, qpc)
        if pos <= 0:
            return samples[0][2]
        if pos >= len(samples):
            return samples[-1][2]
        q0, _t0, p0 = samples[pos - 1]
        q1, _t1, p1 = samples[pos]
        if q1 <= q0:
            return p0
        fraction = (qpc - q0) / float(q1 - q0)
        return p0 + fraction * (p1 - p0)

    def _local_velocity(qpc):
        if qpc is None:
            return None, 0
        start_qpc = qpc - int(round(velocity_window_s * qpc_frequency))
        lo = bisect.bisect_left(qpcs, start_qpc)
        hi = bisect.bisect_right(qpcs, qpc)
        window = samples[lo:hi]
        if len(window) < 3:
            return None, len(window)
        times = [(item[0] - qpc) / qpc_frequency for item in window]
        positions = [item[2] for item in window]
        mean_t = sum(times) / len(times)
        mean_p = sum(positions) / len(positions)
        denominator = sum((value - mean_t) ** 2 for value in times)
        if denominator <= 0.0:
            return None, len(window)
        slope = sum(
            (time_value - mean_t) * (position - mean_p)
            for time_value, position in zip(times, positions)
        ) / denominator
        return slope, len(window)

    schedule = list(getattr(state, "target_speed_schedule", None) or [])
    radius = float(getattr(state, "raio_mm", 0.0) or 0.0)
    ratio = float(getattr(state, "relacao", 0.0) or 0.0)
    course = float(getattr(state, "reciprocating_course_mm", 0.0) or 0.0)

    def _fmt(value, digits=9):
        if value is None or not math.isfinite(float(value)):
            return "NULL"
        return f"{float(value):.{digits}f}"

    rows = []
    previous_extreme = 0.0
    count = max(len(events["legacy"]), len(events["external"]))
    for index in range(count):
        legacy = events["legacy"][index] if index < len(events["legacy"]) else {}
        completion = events["completion"][index] if index < len(events["completion"]) else {}
        external = events["external"][index] if index < len(events["external"]) else {}
        physical = events["physical"][index] if index < len(events["physical"]) else {}
        legacy_idx = _number(legacy.get("idx"), int)
        legacy_qpc = _number(legacy.get("qpc"), int)
        if legacy_qpc is None and legacy_idx is not None:
            legacy_qpc = drive_qpc.get(legacy_idx)
        external_qpc = _number(external.get("qpc"), int)
        physical_qpc = _number(physical.get("qpc"), int)
        direction = _number(legacy.get("dir"), int)
        if direction not in (-1, 1):
            new_direction = _number(external.get("new_direction"), int)
            direction = -new_direction if new_direction in (-1, 1) else None
        legacy_position = _interpolated_position(legacy_qpc)
        external_position = _number(external.get("position_mm"), float)
        physical_extreme = _number(physical.get("extreme_mm"), float)
        control_qpc = external_qpc if external_qpc is not None else legacy_qpc
        velocity_signed, velocity_samples = _local_velocity(control_qpc)
        segment = _number(completion.get("completed_segment"), int)
        target_speed = None
        if segment is not None and 0 <= segment < len(schedule):
            target_speed = abs(_number(schedule[segment][0], float, 0.0))
        if target_speed is None:
            target_speed = _number(legacy.get("target_speed_mm_s"), float)
        target_rpm = _number(legacy.get("target_rpm_abs"), int)
        if target_rpm is None:
            target_rpm = _number(completion.get("target_rpm_abs"), int)
        if target_speed is None and target_rpm is not None and radius > 0.0 and ratio > 0.0:
            target_speed = target_rpm * (2.0 * math.pi * radius) / (60.0 * ratio)
        target_position = (
            course if direction == 1 else 0.0 if direction == -1 else None
        )
        physical_course = None
        if physical_extreme is not None:
            physical_course = abs(physical_extreme - previous_extreme)
            previous_extreme = physical_extreme
        stop_distance = (
            direction * (physical_extreme - legacy_position)
            if direction in (-1, 1) and physical_extreme is not None and legacy_position is not None
            else None
        )
        remaining_external = (
            direction * (physical_extreme - external_position)
            if direction in (-1, 1) and physical_extreme is not None and external_position is not None
            else None
        )
        def _delta_ms(end, start):
            return 1000.0 * (end - start) / qpc_frequency if end is not None and start is not None else None
        rows.append({
            "fase": phase,
            "stroke": index + 1,
            "curso_configurado_mm": _fmt(course),
            "sentido": direction if direction is not None else "NULL",
            "velocidade_alvo_mm_s": _fmt(target_speed),
            "rpm_alvo_drive": target_rpm if target_rpm is not None else "NULL",
            "janela_velocidade_externa_ms": _fmt(1000.0 * velocity_window_s, 3),
            "amostras_velocidade_externa": velocity_samples,
            "velocidade_externa_assinada_mm_s": _fmt(velocity_signed),
            "velocidade_externa_modulo_mm_s": _fmt(abs(velocity_signed) if velocity_signed is not None else None),
            "gatilho_legado_idx": legacy_idx if legacy_idx is not None else "NULL",
            "gatilho_legado_qpc": legacy_qpc if legacy_qpc is not None else "NULL",
            "gatilho_legado_t_s": _fmt((legacy_qpc - qpc_origin) / qpc_frequency if legacy_qpc is not None else None),
            "gatilho_legado_pos_ext_mm": _fmt(legacy_position),
            "alvo_fixo_mm": _fmt(target_position),
            "distancia_ate_alvo_no_gatilho_legado_mm": _fmt(
                direction * (target_position - legacy_position)
                if direction in (-1, 1) and target_position is not None and legacy_position is not None else None
            ),
            "gatilho_externo_idx": _number(external.get("idx"), int, "NULL"),
            "gatilho_externo_qpc": external_qpc if external_qpc is not None else "NULL",
            "gatilho_externo_t_s": _fmt((external_qpc - qpc_origin) / qpc_frequency if external_qpc is not None else None),
            "gatilho_externo_pos_mm": _fmt(external_position),
            "defasagem_externo_legado_ms": _fmt(_delta_ms(external_qpc, legacy_qpc), 6),
            "latencia_observacao_externo_ms": _fmt(_number(external.get("observation_latency_ms"), float), 6),
            "extremo_fisico_idx": _number(physical.get("idx"), int, "NULL"),
            "extremo_fisico_qpc": physical_qpc if physical_qpc is not None else "NULL",
            "extremo_fisico_t_s": _fmt((physical_qpc - qpc_origin) / qpc_frequency if physical_qpc is not None else None),
            "extremo_fisico_mm": _fmt(physical_extreme),
            "curso_fisico_mm": _fmt(physical_course),
            "distancia_parada_apos_gatilho_legado_mm": _fmt(stop_distance),
            "tempo_parada_apos_gatilho_legado_ms": _fmt(_delta_ms(physical_qpc, legacy_qpc), 6),
            "distancia_restante_apos_gatilho_externo_mm": _fmt(remaining_external),
            "tempo_extremo_apos_gatilho_externo_ms": _fmt(_delta_ms(physical_qpc, external_qpc), 6),
            "latencia_observacao_extremo_ms": _fmt(_number(physical.get("observation_latency_ms"), float), 6),
        })
    if output_path is None:
        output_path = os.path.join(rep_dir, "recip_stop_diagnostics.csv")
    try:
        os.makedirs(os.path.dirname(output_path), exist_ok=True)
        with open(output_path, "w", newline="", encoding="ascii") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0].keys()) if rows else [], delimiter=";", lineterminator="\n")
            if rows:
                writer.writeheader()
                writer.writerows(rows)
    except Exception:
        return {}
    complete = sum(1 for row in rows if row["extremo_fisico_mm"] != "NULL")
    summary = {
        "path": output_path,
        "rows": len(rows),
        "complete_rows": complete,
        "velocity_method": "ols_posicao_externa_250ms_antes_gatilho_de_controle",
        "diagnostic_only": True,
    }
    state.reciprocating_stop_diagnostics = summary
    return summary


def _monitor_continuous_encoder_events(state: RunState) -> None:
    """Converte eventos causais do DLG em parada do ensaio continuo.

    O calculo e a decisao do alvo ocorrem no processo C que recebe o CH3. Esta
    thread apenas retransmite STOP aos dois loggers; ela nao calcula posicao a
    partir de CSV nem participa do modo reciprocante.
    """
    stream = getattr(state.dlg_proc, "stdout", None)
    if stream is None:
        state.continuous_encoder_status = "falha_sem_stdout_dlg"
        state.continuous_encoder_message = "stdout do DLG indisponivel"
        _send_ipc(state.drive_proc, "STOP")
        _send_ipc(state.dlg_proc, "STOP")
        return
    try:
        for raw_line in iter(stream.readline, ""):
            line = raw_line.strip()
            if line.startswith("ENCODER_TARGET_REACHED"):
                fields = _event_fields(line)
                state.continuous_encoder_status = "alvo_atingido"
                state.continuous_encoder_message = line
                try:
                    state.continuous_encoder_target_t_s = float(fields.get("t_s", "nan"))
                except Exception:
                    state.continuous_encoder_target_t_s = None
                _send_ipc(state.drive_proc, "STOP")
                _send_ipc(state.dlg_proc, "STOP")
                return
            if line.startswith("ENCODER_FAILED"):
                fields = _event_fields(line)
                reason = fields.get("reason", "ENCODER_HEALTH_FAILED")
                status_by_reason = {
                    "DIRECTION_LOCK_TIMEOUT": "falha_sentido_timeout",
                    "DIRECTION_INCONSISTENT": "falha_sentido_incompativel",
                    "ENCODER_HEALTH_FAILED": "falha_encoder_3s",
                }
                state.continuous_encoder_status = status_by_reason.get(
                    reason, "falha_encoder"
                )
                state.continuous_encoder_message = line
                _send_ipc(state.drive_proc, "STOP")
                _send_ipc(state.dlg_proc, "STOP")
                return
    except Exception as exc:
        state.continuous_encoder_status = "falha_monitor_encoder"
        state.continuous_encoder_message = str(exc)
        _send_ipc(state.drive_proc, "STOP")
        _send_ipc(state.dlg_proc, "STOP")
        return
    if state.continuous_encoder_status == "aguardando_alvo":
        if state.manual_stop_requested:
            state.continuous_encoder_status = "parada_manual"
            state.continuous_encoder_message = "ensaio interrompido manualmente"
        else:
            state.continuous_encoder_status = "deadline_sem_alvo"
            state.continuous_encoder_message = "DLG encerrou antes da distancia alvo"
        _send_ipc(state.drive_proc, "STOP")


def sanitize_folder_name(name: str) -> str:
    """
    Normaliza um nome de pasta para ser aceito no Windows.

    Regras aplicadas:
    - Substitui caracteres reservados por "_".
    - Remove ponto/espaco no final (nao permitido no Windows).

    Parametros:
        name: nome sugerido para pasta.

    Retorna:
        Nome seguro para criacao de diretorio.
    """
    invalid = '<>:"/\\\\|?*'
    out = ''.join('_' if c in invalid else c for c in name)
    out = out.rstrip(' .')
    return out if out else "Ensaio"


def build_output_paths(base_dir: str, nome_ensaio: str, estudo: str) -> dict:
    """
    Monta a estrutura padrao de saida para um ensaio.

    Fluxo:
    1) Gera nome de pasta com sanitize_folder_name().
    2) Cria a pasta do ensaio.
    3) Retorna caminhos padrao dos CSVs usados no pipeline.

    Parametros:
        base_dir: pasta base (ex.: Desktop\\Repositorio).
        nome_ensaio: nome informado no formulario.
        estudo: identificador de estudo/repeticao.

    Retorna:
        dict com caminhos: folder, info_csv, dlg_csv, drive_csv, turn_dist_csv, turn_vp_csv, merge_csv, schedule_csv.

    Excecoes:
        FileExistsError: quando a pasta ja existe (evita sobrescrever ensaio).
    """
    folder_name = sanitize_folder_name(f"{nome_ensaio} - {estudo}")
    folder_path = os.path.join(base_dir, folder_name)
    if os.path.exists(folder_path):
        raise FileExistsError(folder_path)
    os.makedirs(folder_path, exist_ok=True)

    return {
        "folder": folder_path,
        "info_csv": os.path.join(folder_path, "info_ensaio.csv"),
        "dlg_csv": os.path.join(folder_path, "dlg.csv"),
        "dlg_compat_csv": os.path.join(folder_path, "dlg_compat_50hz.csv"),
        "encoder_state_csv": os.path.join(folder_path, "encoder_state.csv"),
        "drive_csv": os.path.join(folder_path, "drive.csv"),
        "turn_dist_csv": os.path.join(folder_path, "atrito_por_distancia.csv"),
        "turn_vp_csv": os.path.join(folder_path, "atrito_por_volta.csv"),
        "merge_csv": os.path.join(folder_path, "resultado_ensaio.csv"),
        "schedule_csv": os.path.join(folder_path, "schedule.csv"),
    }


def write_schedule_csv(path: str, schedule: List[Tuple[int, float]]) -> None:
    """
    Escreve o cronograma do Drive em CSV.

    Formato esperado:
    - cabecalho: rpm,duration_s
    - linhas: (rpm, duracao_em_segundos)

    Parametros:
        path: caminho de saida do schedule.csv.
        schedule: lista de tuplas (rpm, duration_s).
    """
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["rpm", "duration_s"])
        for rpm, dur_s in schedule:
            w.writerow([rpm, f"{dur_s:.6f}"])


def rpm_from_mm_s(vel_mm_s: float, raio_mm: float, relacao: float = 1.0) -> int:
    """
    Converte velocidade linear (mm/s) para RPM.

    Formula (i = D2 / D1):
        D1 = diametro da polia do motor
        D2 = diametro da polia do disco
        rpm_motor = i * (v_pino * 60) / (2*pi*raio_pino)
    onde:
        i = relacao mecanica
        v = velocidade linear em mm/s
        raio = raio em mm

    Parametros:
        vel_mm_s: velocidade linear.
        raio_mm: raio do movimento.
        relacao: relacao mecanica i = D2 / D1.

    Retorna:
        RPM inteiro arredondado para uso no Drive.
    """
    if raio_mm <= 0 or relacao <= 0:
        return 0
    # No fluxo atual, enviamos somente magnitude de velocidade.
    # Inversao de sentido e tratada pela sequencia mecanica, nao por RPM negativo.
    rpm = abs((vel_mm_s * 60.0 * relacao) / (2.0 * 3.141592653589793 * raio_mm))
    # Arredonda para inteiro mais proximo (Drive recebe setpoint inteiro).
    return int(rpm + 0.5 if rpm >= 0 else rpm - 0.5)


def pin_speed_mm_s_from_rpm(rpm_motor: float, raio_mm: float, relacao: float = 1.0) -> Optional[float]:
    """
    Converte RPM do motor para velocidade linear real no pino (mm/s).

    Formula inversa de rpm_from_mm_s:
        v_pino = |rpm_motor| * (2*pi*raio_pino) / (60*i)
    onde i = D2 / D1.

    Retorna None quando parametros geometricos forem invalidos.
    """
    try:
        rpm = float(rpm_motor)
        raio = float(raio_mm)
        i = float(relacao)
    except Exception:
        return None
    if raio <= 0 or i <= 0:
        return None
    return abs(rpm) * (2.0 * 3.141592653589793 * raio) / (60.0 * i)


def find_exe(candidates: List[str], fallback_name: Optional[str] = None) -> Optional[str]:
    """
    Procura um executavel em uma lista de caminhos candidatos.

    Fluxo:
    1) Retorna o primeiro caminho existente em `candidates`.
    2) Se nao achar e houver `fallback_name`, tenta localizar via PATH.

    Parametros:
        candidates: lista de caminhos absolutos/relativos esperados.
        fallback_name: nome do executavel para busca em PATH.

    Retorna:
        Caminho encontrado ou None.
    """
    for c in candidates:
        if c and os.path.exists(c):
            return c
    if fallback_name:
        return shutil.which(fallback_name)
    return None


def find_repo_root(start: Optional[Path] = None) -> str:
    """
    Localiza a raiz do repositorio subindo diretorios.

    Criterio de deteccao:
    - Diretorio que contenha simultaneamente `DLG4000` e `DriveA5`.

    Observacao:
    - Funciona tanto em execucao por fonte (.py) quanto em executavel
      empacotado (PyInstaller / sys.frozen).

    Parametros:
        start: caminho inicial opcional para iniciar a busca.

    Retorna:
        Caminho da raiz do repo, ou cwd como alternativa.
    """
    if start is None:
        if getattr(sys, "frozen", False):
            start = Path(sys.executable).resolve()
        else:
            start = Path(__file__).resolve()
    if start.is_file():
        start = start.parent

    for parent in [start] + list(start.parents):
        if (parent / "DLG4000").exists() and (parent / "DriveA5").exists():
            return str(parent)
    # Alternativa: usa o diretorio atual para evitar excecao.
    return os.getcwd()


def check_executables(repo_root: str) -> dict:
    """
    Encontra os executaveis obrigatorios do pipeline externo.

    Executaveis esperados:
    - dlg_logger_ipc.exe
    - a5_speed_logger.exe
    - merge_logs.exe (opcional, apenas para compatibilidade legada)

    Parametros:
        repo_root: raiz do repositorio.

    Retorna:
        dict com:
        - dlg_exe, drive_exe, merge_exe: caminhos resolvidos (ou None)
        - missing: lista de descricoes dos executaveis nao encontrados
    """
    dlg_exe = find_exe([
        os.path.join(repo_root, "DLG4000", "bin", "Release", "dlg_logger_ipc.exe"),
        os.path.join(repo_root, "DLG4000", "bin", "dlg_logger_ipc.exe"),
    ], "dlg_logger_ipc.exe")

    drive_exe = find_exe([
        os.path.join(repo_root, "DriveA5", "build_vs2022", "Release", "a5_speed_logger.exe"),
        os.path.join(repo_root, "DriveA5", "build", "Release", "a5_speed_logger.exe"),
        os.path.join(repo_root, "DriveA5", "build", "a5_speed_logger.exe"),
    ], "a5_speed_logger.exe")

    merge_exe = find_exe([
        os.path.join(repo_root, "DriveA5", "build_vs2022", "Release", "merge_logs.exe"),
        os.path.join(repo_root, "DriveA5", "build", "Release", "merge_logs.exe"),
        os.path.join(repo_root, "DriveA5", "build", "merge_logs.exe"),
    ], "merge_logs.exe")

    missing = []
    if not dlg_exe:
        missing.append("dlg_logger_ipc.exe (DLG4000/bin/Release)")
    if not drive_exe:
        missing.append("a5_speed_logger.exe (DriveA5/build/Release)")
    return {
        "dlg_exe": dlg_exe,
        "drive_exe": drive_exe,
        "turn_exe": "",
        "merge_exe": merge_exe,
        "missing": missing,
    }


def find_calibra_ui_exe(repo_root: str = "") -> Optional[str]:
    """
    Localiza o executavel CalibraDLG_UI.exe para o botao "Configurar canais".

    Parametros:
        repo_root: raiz do repositorio (opcional).

    Retorna:
        Caminho do executavel ou None.
    """
    candidates = []
    if repo_root:
        candidates.extend([
            os.path.join(repo_root, "CalibraDLG_UI", "bin", "Release", "net6.0-windows", "CalibraDLG_UI.exe"),
            os.path.join(repo_root, "CalibraDLG_UI.exe"),
        ])

    return find_exe(candidates, "CalibraDLG_UI.exe")


def start_external_run(
    repo_root: str,
    out_paths: dict,
    schedule: List[Tuple[int, float]],
    duration_s: float,
    rate_hz: float = DEFAULT_RATE_HZ,
    dlg_ip: str = DEFAULT_DLG_IP,
    dlg_port: int = DEFAULT_DLG_PORT,
    bind_ip: str = DEFAULT_BIND_IP,
    bind_port: int = DEFAULT_BIND_PORT,
    com_port: str = "COM5",
    slave_id: int = 1,
    baud: int = 115200,
    parity: str = "E",
    show_console: bool = False,
    force_normal_n: float = 0.0,
    relacao: float = 1.0,
    raio_mm: float = 0.0,
    distance_interval_mm: float = 10.0,
    reciprocating: bool = False,
    reciprocating_course_mm: float = 0.0,
    reciprocating_total_mm: float = 0.0,
    reciprocating_tolerance_counts: int = 0,
    reciprocating_edge_filter_pct: float = 0.0,
    dynamic_offset_n: float = 0.0,
    target_speed_schedule: Optional[List[Tuple[float, float]]] = None,
    drive_rate_hz: float = DEFAULT_DRIVE_RATE_HZ,
    continuous_target_mm: float = 0.0,
    reciprocating_shadow_enabled: bool = False,
    reciprocating_shadow_tolerance_mm: float = 0.5,
    reciprocating_shadow_total_mm: float = 0.0,
    reciprocating_shadow_forward_sign: int = 0,
    reciprocating_shadow_stop_compensation: bool = False,
    reciprocating_shadow_stop_slope_s: float = RECIP_SHADOW_STOP_MODEL_SLOPE_S,
    reciprocating_shadow_stop_margin_mm: float = RECIP_SHADOW_STOP_MARGIN_MM,
    reciprocating_shadow_stop_velocity_window_s: float = RECIP_SHADOW_STOP_VELOCITY_WINDOW_S,
    reciprocating_shadow_stop_max_course_fraction: float = RECIP_SHADOW_STOP_MAX_COURSE_FRACTION,
    strict_drive_setup: bool = False,
    reciprocating_encoder_control_enabled: bool = False,
    drive_command_only: bool = False,
) -> RunState:
    """
    Inicia o ensaio em modo externo (executaveis C sem interface).

    Fluxo resumido:
    1) Escreve schedule.csv.
    2) Encontra executaveis obrigatorios.
    3) Sobe logger DLG e logger Drive com --ipc.
    4) Aguarda READY.
    5) Envia START no DLG.
    6) Aguarda DLG sinalizar primeira amostra valida (DATA_OK).
    7) Envia START no Drive.

    A ordem DLG -> Drive reduz risco de o motor iniciar sem aquisicao valida.
    Ocasiando menos percas de dados por aquisicoes com problemas.
    Parametros:
        repo_root: raiz do repositorio para localizar .exe.
        out_paths: caminhos de saida (dlg_csv, drive_csv, turn_dist_csv, turn_vp_csv, merge_csv, schedule_csv).
        schedule: lista de etapas (rpm, duracao_s).
        duration_s: duracao total do ensaio.
        rate_hz: taxa alvo de aquisicao completa do DLG.
        drive_rate_hz: taxa do Drive e da trilha DLG compatível usada pelo
            processamento legado baseado em indices.
        dlg_ip/dlg_port: destino UDP do DLG.
        bind_ip/bind_port: bind local para recebimento UDP.
        com_port/slave_id/baud/parity: parametros seriais do Drive.
        show_console: quando False, tenta ocultar janela de console no Windows.
        raio_mm: raio da trilha no disco (usado para derivar velocidade media por volta).
        distance_interval_mm: tamanho do intervalo (mm) para agregar arquivo _P.

    Retorna:
        RunState com handles de processo, caminhos e parametros da execucao.

    Excecoes:
        FileNotFoundError: quando algum executavel obrigatorio nao e encontrado.
    """
    if "turn_dist_csv" not in out_paths or not out_paths.get("turn_dist_csv"):
        base_folder = os.path.dirname(out_paths.get("merge_csv", "")) or os.getcwd()
        old_turn = out_paths.get("turn_csv")
        if old_turn:
            out_paths["turn_dist_csv"] = old_turn
        else:
            out_paths["turn_dist_csv"] = os.path.join(base_folder, "atrito_por_distancia.csv")
    if "turn_vp_csv" not in out_paths or not out_paths.get("turn_vp_csv"):
        base_folder = os.path.dirname(out_paths.get("merge_csv", "")) or os.getcwd()
        dist_name = os.path.basename(out_paths.get("turn_dist_csv", "")) or ""
        movement_suffix = "_M.csv" if reciprocating else "_VP.csv"
        if dist_name.endswith("_DP.csv"):
            out_paths["turn_vp_csv"] = os.path.join(base_folder, dist_name.replace("_DP.csv", movement_suffix))
        else:
            fallback_name = "atrito_por_stroke.csv" if reciprocating else "atrito_por_volta.csv"
            out_paths["turn_vp_csv"] = os.path.join(base_folder, fallback_name)
    base_folder = os.path.dirname(out_paths.get("dlg_csv", "")) or os.getcwd()
    if "dlg_compat_csv" not in out_paths or not out_paths.get("dlg_compat_csv"):
        out_paths["dlg_compat_csv"] = os.path.join(base_folder, "dlg_compat_50hz.csv")
    if "encoder_state_csv" not in out_paths or not out_paths.get("encoder_state_csv"):
        out_paths["encoder_state_csv"] = os.path.join(base_folder, "encoder_state.csv")
    if rate_hz <= 0.0 or drive_rate_hz <= 0.0 or drive_rate_hz > rate_hz:
        raise ValueError("Taxas invalidas: DLG deve ser >= Drive e ambas positivas.")
    rate_ratio = rate_hz / drive_rate_hz
    if abs(rate_ratio - round(rate_ratio)) > 1.0e-9:
        raise ValueError("A taxa do DLG deve ser multiplo inteiro da taxa do Drive.")
    if reciprocating:
        current_motion = out_paths.get("turn_vp_csv", "")
        base_folder = os.path.dirname(current_motion) or os.path.dirname(out_paths.get("merge_csv", "")) or os.getcwd()
        current_name = os.path.basename(current_motion)
        if current_name.endswith("_VP.csv"):
            out_paths["turn_vp_csv"] = os.path.join(base_folder, current_name.replace("_VP.csv", "_M.csv"))
        elif current_name == "atrito_por_volta.csv":
            out_paths["turn_vp_csv"] = os.path.join(base_folder, "atrito_por_stroke.csv")

    write_schedule_csv(out_paths["schedule_csv"], schedule)

    # Encontra executaveis a partir da raiz do repositorio.
    if not repo_root:
        repo_root = find_repo_root()
    exe_info = check_executables(repo_root)
    dlg_exe = exe_info["dlg_exe"]
    drive_exe = exe_info["drive_exe"]
    merge_exe = exe_info["merge_exe"]
    if exe_info["missing"]:
        raise FileNotFoundError(
            "Executaveis nao encontrados: " + "; ".join(exe_info["missing"])
        )

    creationflags = 0
    if not show_console and hasattr(subprocess, "CREATE_NO_WINDOW"):
        creationflags = subprocess.CREATE_NO_WINDOW

    # Sobe logger do DLG em modo IPC (aguarda START no stdin).
    dlg_cmd = [
        dlg_exe,
        "--out", out_paths["dlg_csv"],
        "--duration", f"{duration_s:.6f}",
        "--rate", f"{rate_hz:.6f}",
        "--force-normal", f"{force_normal_n:.6f}",
        "--ip", dlg_ip,
        "--port", str(dlg_port),
        "--bind-ip", bind_ip,
        "--bind-port", str(bind_port),
    ]
    if abs(rate_hz - drive_rate_hz) > 1.0e-9 and not (
        drive_command_only or reciprocating_encoder_control_enabled
    ):
        dlg_cmd.extend([
            "--compat-out", out_paths["dlg_compat_csv"],
            "--compat-rate", f"{drive_rate_hz:.6f}",
        ])
    else:
        out_paths["dlg_compat_csv"] = out_paths["dlg_csv"]
    encoder_speed_limits = [
        abs(float(item[0])) for item in (target_speed_schedule or [])
        if item and len(item) >= 1 and float(item[0]) != 0.0
    ]
    shadow_port = 0
    shadow_session = 0
    shadow_total_mm = 0.0
    shadow_stroke_timeout_s = 0.0
    encoder_recip_link_enabled = bool(
        reciprocating and
        (reciprocating_shadow_enabled or reciprocating_encoder_control_enabled)
    )
    if encoder_recip_link_enabled:
        if int(reciprocating_shadow_forward_sign) not in (-1, 0, 1):
            raise ValueError("Sentido preaprendido do encoder deve ser -1, 0 ou 1.")
        if raio_mm <= 0.0 or reciprocating_course_mm <= 0.0:
            raise ValueError("Modo sombra reciprocante exige raio e curso validos.")
        if not encoder_speed_limits:
            encoder_speed_limits = [
                abs(float(rpm)) * (2.0 * math.pi * float(raio_mm)) /
                (60.0 * float(relacao))
                for rpm, _duration in schedule
                if float(rpm) != 0.0 and float(relacao) > 0.0
            ]
        if not encoder_speed_limits:
            raise ValueError("Modo sombra reciprocante exige velocidade valida.")
        min_speed_mm_s = min(encoder_speed_limits)
        expected_stroke_s = float(reciprocating_course_mm) / min_speed_mm_s
        shadow_stroke_timeout_s = max(
            expected_stroke_s * 2.0,
            expected_stroke_s + 10.0,
        )
        shadow_total_mm = float(
            reciprocating_shadow_total_mm
            if reciprocating_shadow_total_mm > 0.0
            else reciprocating_total_mm
        )
        if shadow_total_mm <= 0.0 or reciprocating_shadow_tolerance_mm < 0.0:
            raise ValueError("Distancia/tolerancia do modo sombra reciprocante invalida.")
        if reciprocating_shadow_stop_compensation:
            if (
                not math.isfinite(float(reciprocating_shadow_stop_slope_s))
                or float(reciprocating_shadow_stop_slope_s) < 0.0
                or not math.isfinite(float(reciprocating_shadow_stop_margin_mm))
                or float(reciprocating_shadow_stop_margin_mm) < 0.0
                or not math.isfinite(float(reciprocating_shadow_stop_velocity_window_s))
                or float(reciprocating_shadow_stop_velocity_window_s) <= 0.0
                or not math.isfinite(float(reciprocating_shadow_stop_max_course_fraction))
                or not 0.0 < float(reciprocating_shadow_stop_max_course_fraction) < 1.0
            ):
                raise ValueError("Parametros da compensacao de parada do modo sombra invalidos.")
        shadow_port = _reserve_loopback_udp_port()
        shadow_session = secrets.randbits(63) or 1
        # Margem para transitorios e ultrapassagens; o gate continua limitado
        # pelas regras fisicas internas do EncoderCore.
        shadow_max_speed_mm_s = max(5.0, max(encoder_speed_limits) * 2.0)
        dlg_cmd.extend([
            "--encoder-state-out", out_paths["encoder_state_csv"],
            "--encoder-radius-mm", f"{raio_mm:.9f}",
            "--encoder-max-speed-mm-s", f"{shadow_max_speed_mm_s:.9f}",
            "--encoder-target-mm", "0",
            "--encoder-control-port", str(shadow_port),
            "--encoder-session", str(shadow_session),
            "--encoder-reciprocating-shadow",
        ])
    if not reciprocating and continuous_target_mm > 0.0:
        if raio_mm <= 0.0 or not encoder_speed_limits:
            raise ValueError("Modo continuo pelo encoder exige raio e velocidade alvo validos.")
        effective_speed_limits = [
            abs(float(rpm)) * (2.0 * math.pi * float(raio_mm)) /
            (60.0 * float(relacao))
            for rpm, _duration in schedule
            if float(rpm) != 0.0 and float(relacao) > 0.0
        ]
        # O Drive recebe RPM inteiro. O gate fisico precisa considerar essa
        # velocidade efetivamente comandada, com 2x de margem para transitorios.
        continuous_max_speed_mm_s = max(
            5.0,
            max(encoder_speed_limits + effective_speed_limits) * 2.0,
        )
        dlg_cmd.extend([
            "--encoder-state-out", out_paths["encoder_state_csv"],
            "--encoder-radius-mm", f"{raio_mm:.9f}",
            "--encoder-max-speed-mm-s", f"{continuous_max_speed_mm_s:.9f}",
            "--encoder-target-mm", f"{continuous_target_mm:.9f}",
        ])
    dlg_cmd.append("--ipc")
    dlg_proc = subprocess.Popen(
        dlg_cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        creationflags=creationflags,
    )

    # Sobe logger do Drive em modo IPC (aguarda START no stdin).
    drive_cmd = [
        drive_exe,
        "--port", com_port,
        "--out", out_paths["drive_csv"],
        "--schedule", out_paths["schedule_csv"],
        "--duration", f"{duration_s:.6f}",
        "--rate", f"{drive_rate_hz:.6f}",
        "--slave", str(slave_id),
        "--baud", str(baud),
        "--parity", parity,
        "--setup",
        "--ipc",
    ]
    if drive_command_only or reciprocating_encoder_control_enabled:
        drive_cmd.append("--command-only")
    if strict_drive_setup:
        drive_cmd.append("--strict-setup")
    if reciprocating:
        drive_cmd.extend([
            "--reciprocating",
            "--recip-course-mm", f"{reciprocating_course_mm:.6f}",
            "--recip-total-mm", f"{reciprocating_total_mm:.6f}",
            "--recip-radius-mm", f"{raio_mm:.6f}",
            "--recip-ratio", f"{relacao:.6f}",
            "--recip-tol-counts", str(int(reciprocating_tolerance_counts)),
        ])
        if encoder_recip_link_enabled:
            drive_cmd.extend([
                (
                    "--recip-encoder-control"
                    if reciprocating_encoder_control_enabled
                    else "--recip-encoder-shadow"
                ),
                "--recip-shadow-port", str(shadow_port),
                "--recip-shadow-session", str(shadow_session),
                "--recip-shadow-tol-mm", f"{reciprocating_shadow_tolerance_mm:.9f}",
                "--recip-shadow-total-mm", f"{shadow_total_mm:.9f}",
                "--recip-shadow-stroke-timeout-s", f"{shadow_stroke_timeout_s:.9f}",
                "--recip-shadow-forward-sign", str(int(reciprocating_shadow_forward_sign)),
            ])
            if reciprocating_shadow_stop_compensation:
                drive_cmd.extend([
                    "--recip-shadow-stop-compensation",
                    "--recip-shadow-stop-slope-s", f"{reciprocating_shadow_stop_slope_s:.12f}",
                    "--recip-shadow-stop-margin-mm", f"{reciprocating_shadow_stop_margin_mm:.12f}",
                    "--recip-shadow-stop-velocity-window-s", f"{reciprocating_shadow_stop_velocity_window_s:.9f}",
                    "--recip-shadow-stop-max-course-fraction", f"{reciprocating_shadow_stop_max_course_fraction:.9f}",
                ])
    drive_proc = subprocess.Popen(
        drive_cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        creationflags=creationflags,
    )

    try:
        # READY obrigatorio: se algum logger nao iniciar corretamente, aborta o ensaio.
        if not _wait_ready(dlg_proc, "DLG"):
            raise RuntimeError("DLG logger nao respondeu READY.")
        if not _wait_ready(drive_proc, "Drive"):
            raise RuntimeError("Drive logger nao respondeu READY.")

        # Sequencia de start:
        # 1) DLG inicia e confirma primeira amostra.
        # 2) Drive inicia depois (sincronismo mais confiavel).
        _send_start(dlg_proc)
        # Nao aborta aqui em DATA_TIMEOUT: o supervisório ja valida amostras
        # reais antes de iniciar cronometro e pode abortar de forma controlada
        # se o DLG nao estabilizar.
        _wait_data_ready(dlg_proc, timeout_s=6.0)
        _send_start(drive_proc)
    except Exception:
        # Evita processos zumbis se startup falhar em qualquer etapa.
        for p in (dlg_proc, drive_proc):
            try:
                if p and p.poll() is None:
                    _send_ipc(p, "STOP")
            except Exception:
                pass
        time.sleep(0.2)
        for p in (dlg_proc, drive_proc):
            try:
                if p and p.poll() is None:
                    p.terminate()
            except Exception:
                pass
        raise

    state = RunState(
        dlg_proc=dlg_proc,
        drive_proc=drive_proc,
        turn_proc=None,
        dlg_csv=out_paths["dlg_csv"],
        dlg_compat_csv=out_paths["dlg_compat_csv"],
        encoder_state_csv=out_paths["encoder_state_csv"],
        drive_csv=out_paths["drive_csv"],
        turn_dist_csv=out_paths["turn_dist_csv"],
        turn_vp_csv=out_paths["turn_vp_csv"],
        merge_csv=out_paths["merge_csv"],
        schedule_csv=out_paths["schedule_csv"],
        dlg_exe=dlg_exe,
        drive_exe=drive_exe,
        turn_exe="",
        merge_exe=merge_exe,
        duration_s=duration_s,
        rate_hz=rate_hz,
        drive_rate_hz=drive_rate_hz,
        force_normal_n=force_normal_n,
        relacao=relacao,
        raio_mm=raio_mm,
        distance_interval_mm=distance_interval_mm,
        reciprocating=bool(reciprocating),
        reciprocating_course_mm=reciprocating_course_mm,
        reciprocating_total_mm=reciprocating_total_mm,
        reciprocating_tolerance_counts=int(reciprocating_tolerance_counts),
        reciprocating_edge_filter_pct=float(reciprocating_edge_filter_pct),
        reciprocating_shadow_enabled=bool(reciprocating and reciprocating_shadow_enabled),
        reciprocating_encoder_control_enabled=bool(
            reciprocating and reciprocating_encoder_control_enabled
        ),
        drive_command_only=bool(
            drive_command_only or reciprocating_encoder_control_enabled
        ),
        reciprocating_shadow_port=int(shadow_port),
        reciprocating_shadow_session=int(shadow_session),
        reciprocating_shadow_tolerance_mm=float(reciprocating_shadow_tolerance_mm),
        reciprocating_shadow_total_mm=float(shadow_total_mm),
        reciprocating_shadow_stroke_timeout_s=float(shadow_stroke_timeout_s),
        reciprocating_shadow_forward_sign=int(reciprocating_shadow_forward_sign),
        reciprocating_shadow_stop_compensation=bool(
            encoder_recip_link_enabled and
            reciprocating_shadow_stop_compensation
        ),
        reciprocating_shadow_stop_slope_s=float(reciprocating_shadow_stop_slope_s),
        reciprocating_shadow_stop_margin_mm=float(reciprocating_shadow_stop_margin_mm),
        reciprocating_shadow_stop_velocity_window_s=float(
            reciprocating_shadow_stop_velocity_window_s
        ),
        reciprocating_shadow_stop_max_course_fraction=float(
            reciprocating_shadow_stop_max_course_fraction
        ),
        dynamic_offset_n=float(dynamic_offset_n),
        target_speed_schedule=list(target_speed_schedule or []),
        continuous_target_mm=float(continuous_target_mm),
        continuous_encoder_status=(
            "aguardando_alvo" if not reciprocating and continuous_target_mm > 0.0
            else "nao_aplicavel"
        ),
    )
    if state.continuous_encoder_status == "aguardando_alvo":
        state.encoder_monitor_thread = threading.Thread(
            target=_monitor_continuous_encoder_events,
            args=(state,),
            daemon=True,
            name="continuous-encoder-events",
        )
        state.encoder_monitor_thread.start()
    return state


def run_reciprocating_offset_capture(
    repo_root: str,
    run_folder: str,
    course_mm: float,
    cycles: int,
    relacao: float,
    raio_mm: float,
    tolerance_counts: int,
    rate_hz: float = DEFAULT_RATE_HZ,
    max_loss_pct: float = 5.0,
    progress_cb=None,
) -> dict:
    """Executa a fase preliminar reciprocante e calcula a media assinada de CH1."""
    if course_mm <= 0.0 or cycles <= 0 or relacao <= 0.0 or raio_mm <= 0.0:
        raise ValueError("Parametros invalidos para a correcao dinamica reciprocante.")

    useful_cycles = int(cycles)
    warmup_cycles = 1
    executed_cycles = useful_cycles + warmup_cycles
    useful_strokes = useful_cycles * 2
    target_strokes = executed_cycles * 2
    movement_mm = float(target_strokes) * float(course_mm)
    controller_total_mm = movement_mm
    rpm_disk_target = 1.0
    rpm_drive = max(1, int(round(float(relacao) * rpm_disk_target)))
    rpm_disk_effective = float(rpm_drive) / float(relacao)
    pin_speed_mm_s = rpm_disk_effective * (2.0 * 3.141592653589793 * float(raio_mm)) / 60.0
    theoretical_s = movement_mm / pin_speed_mm_s
    watchdog_s = max(theoretical_s * 1.5, theoretical_s + 30.0)
    dev_dir = os.path.join(run_folder, "DadosDev")
    work_dir = os.path.join(dev_dir, "recip_offset_work")
    os.makedirs(work_dir, exist_ok=True)
    paths = {
        "dlg_csv": os.path.join(work_dir, "dlg.csv"),
        "drive_csv": os.path.join(work_dir, "drive.csv"),
        "turn_dist_csv": os.path.join(work_dir, "unused_DP.csv"),
        "turn_vp_csv": os.path.join(work_dir, "unused_M.csv"),
        "merge_csv": os.path.join(work_dir, "unused_T.csv"),
        "schedule_csv": os.path.join(work_dir, "schedule.csv"),
    }
    state = start_external_run(
        repo_root=repo_root,
        out_paths=paths,
        schedule=[(rpm_drive, watchdog_s)],
        duration_s=watchdog_s,
        rate_hz=rate_hz,
        bind_port=DEFAULT_BIND_PORT,
        com_port="COM5",
        force_normal_n=0.0,
        relacao=relacao,
        raio_mm=raio_mm,
        reciprocating=True,
        reciprocating_course_mm=course_mm,
        reciprocating_total_mm=controller_total_mm,
        reciprocating_tolerance_counts=tolerance_counts,
        reciprocating_shadow_enabled=False,
        reciprocating_encoder_control_enabled=True,
        drive_command_only=True,
        reciprocating_shadow_total_mm=movement_mm,
        reciprocating_shadow_forward_sign=0,
        reciprocating_shadow_stop_compensation=True,
        target_speed_schedule=[(pin_speed_mm_s, watchdog_s)],
    )

    event_path = os.path.join(work_dir, "a5_speed_events.log")

    def _read_recip_events():
        boundary_qpcs = []
        done_qpc = None
        if not os.path.exists(event_path):
            return boundary_qpcs, done_qpc
        try:
            with open(event_path, "r", encoding="utf-8", errors="replace") as f:
                for line in f:
                    if "RECIP_ENCODER_TRIGGER " in line:
                        fields = _event_fields(line)
                        qpc = fields.get("qpc")
                        if qpc is not None:
                            boundary_qpcs.append(int(qpc))
                    elif "RECIP_ENCODER_DONE " in line:
                        fields = _event_fields(line)
                        qpc = fields.get("qpc")
                        if qpc is not None:
                            done_qpc = int(qpc)
                            boundary_qpcs.append(done_qpc)
        except Exception:
            pass
        return boundary_qpcs, done_qpc

    started = time.time()
    target_seen = False
    try:
        while state.drive_proc.poll() is None:
            if progress_cb:
                progress_cb(time.time() - started, theoretical_s)
            live_boundaries, _ = _read_recip_events()
            if len(live_boundaries) >= target_strokes:
                target_seen = True
                # O ultimo RECIP_REVERSE confirma um stroke completo. Encerra
                # imediatamente, sem aguardar o restante do watchdog.
                stop_run(state)
                break
            time.sleep(0.2)
        if not target_seen:
            _send_ipc(state.dlg_proc, "STOP")
        try:
            state.dlg_proc.wait(timeout=5.0)
        except Exception:
            if state.dlg_proc.poll() is None:
                state.dlg_proc.terminate()
                state.dlg_proc.wait(timeout=3.0)
    finally:
        if state.drive_proc.poll() is None or state.dlg_proc.poll() is None:
            stop_run(state)

    drive_rc = state.drive_proc.returncode
    dlg_rc = state.dlg_proc.returncode
    shadow_summary = load_reciprocating_shadow_summary(state)
    encoder_capture_quality = analyze_encoder_state_capture(
        state.encoder_state_csv, movement_mm
    )
    stop_diagnostics_path = os.path.join(work_dir, "recip_stop_diagnostics.csv")
    stop_diagnostics = build_reciprocating_stop_diagnostics(
        state,
        output_path=stop_diagnostics_path,
        phase="correcao_dinamica",
    )
    boundary_qpcs, done_qpc = _read_recip_events()
    del done_qpc
    boundary_qpcs = sorted(set(boundary_qpcs))
    dlg_qpc_idx = []
    if os.path.exists(state.dlg_csv):
        with open(state.dlg_csv, "r", newline="", encoding="utf-8") as stream:
            for row in csv.DictReader(stream):
                try:
                    dlg_qpc_idx.append((int(row["t_qpc"]), int(row["idx"])))
                except Exception:
                    continue
    qpc_values = [item[0] for item in dlg_qpc_idx]
    reverse_indices = []
    for qpc in boundary_qpcs:
        pos = bisect.bisect_left(qpc_values, qpc)
        choices = [candidate for candidate in (pos - 1, pos) if 0 <= candidate < len(dlg_qpc_idx)]
        if choices:
            nearest = min(choices, key=lambda candidate: abs(qpc_values[candidate] - qpc))
            reverse_indices.append(dlg_qpc_idx[nearest][1])
    capture_complete = len(reverse_indices) >= target_strokes
    capture_end_idx = reverse_indices[target_strokes - 1] if capture_complete else None
    retained_start_idx = reverse_indices[1] + 1 if len(reverse_indices) >= 2 else None

    rows = []
    valid_by_idx = {}
    total_rows = 0
    processing_dlg_csv = state.dlg_csv
    if os.path.exists(processing_dlg_csv):
        with open(processing_dlg_csv, "r", newline="", encoding="utf-8") as f:
            reader = csv.reader(f)
            next(reader, None)
            for row in reader:
                if not row:
                    continue
                try:
                    row_idx = int(row[0])
                except Exception:
                    row_idx = None
                in_retained_window = (
                    row_idx is not None and retained_start_idx is not None
                    and capture_end_idx is not None
                    and retained_start_idx <= row_idx <= capture_end_idx
                )
                if in_retained_window:
                    total_rows += 1
                sample_valid = False
                value = None
                try:
                    sample_valid = (
                        len(row) >= 12 and row[-1].strip() == "0"
                        and row[3].strip().upper() != "NULL" and row_idx is not None
                    )
                    if sample_valid:
                        value = float(row[3])
                        valid_by_idx[row_idx] = value
                except Exception:
                    sample_valid = False
                rows.append((
                    row[0] if row else "", row[2] if len(row) > 2 else "",
                    value, sample_valid, in_retained_window,
                ))

    stroke_means = []
    if capture_complete and retained_start_idx is not None:
        stroke_start_idx = retained_start_idx
        for boundary_idx in reverse_indices[2:target_strokes]:
            stroke_values = [
                value for idx, value in valid_by_idx.items()
                if stroke_start_idx <= idx <= boundary_idx
            ]
            stroke_means.append(
                (sum(stroke_values) / len(stroke_values)) if stroke_values else None
            )
            stroke_start_idx = boundary_idx + 1

    cycle_means = []
    if len(stroke_means) == useful_strokes:
        for i in range(0, useful_strokes, 2):
            if stroke_means[i] is None or stroke_means[i + 1] is None:
                cycle_means.append(None)
            else:
                cycle_means.append((stroke_means[i] + stroke_means[i + 1]) / 2.0)

    valid_count = sum(
        1 for idx in valid_by_idx
        if retained_start_idx is not None and capture_end_idx is not None
        and retained_start_idx <= idx <= capture_end_idx
    )
    loss_pct = 100.0 * (total_rows - valid_count) / total_rows if total_rows else 100.0
    valid_cycle_means = [value for value in cycle_means if value is not None]
    offset_n = (
        sum(valid_cycle_means) / len(valid_cycle_means)
        if len(valid_cycle_means) == useful_cycles else 0.0
    )
    primary_failure = _reciprocating_startup_failure_reason(event_path)
    reasons = [primary_failure] if primary_failure else []
    if not primary_failure:
        if drive_rc not in (None, 0):
            reasons.append(f"Drive encerrou com codigo {drive_rc}")
        if dlg_rc not in (None, 0):
            reasons.append(f"DLG encerrou com codigo {dlg_rc}")
        if not capture_complete:
            reasons.append(f"ciclos incompletos ({len(reverse_indices)}/{target_strokes} strokes)")
        if len(valid_cycle_means) != useful_cycles:
            reasons.append(
                f"ciclos sem CH1 valido ({len(valid_cycle_means)}/{useful_cycles})"
            )
        if not valid_count:
            reasons.append("nenhuma amostra CH1 valida")
        if loss_pct > max_loss_pct:
            reasons.append(f"perda DLG {loss_pct:.3f}% acima de {max_loss_pct:.3f}%")

    capture_csv = os.path.join(dev_dir, "recip_dynamic_offset.csv")
    with open(capture_csv, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f, delimiter=";", lineterminator="\n")
        writer.writerow(["idx", "t_s", "ch1_raw_N", "valida", "usada_offset"])
        for idx, t_s, value, sample_valid, in_retained_window in rows:
            writer.writerow([
                idx, t_s, f"{value:.10g}" if value is not None else "NULL",
                1 if sample_valid else 0,
                1 if sample_valid and in_retained_window else 0,
            ])

    for name in ("a5_speed_events.log", "dlg_logger_events.log", "drive.csv", "schedule.csv", "encoder_state.csv"):
        src = os.path.join(work_dir, name)
        if os.path.exists(src):
            shutil.copy2(src, os.path.join(dev_dir, f"recip_offset_{name}"))
    if os.path.exists(state.dlg_csv):
        shutil.copy2(state.dlg_csv, os.path.join(dev_dir, "recip_offset_dlg_200hz.csv"))
    if state.dlg_compat_csv != state.dlg_csv and os.path.exists(state.dlg_compat_csv):
        shutil.copy2(state.dlg_compat_csv, os.path.join(dev_dir, "recip_offset_dlg_compat_50hz.csv"))
    if os.path.exists(stop_diagnostics_path):
        shutil.copy2(
            stop_diagnostics_path,
            os.path.join(dev_dir, "recip_offset_stop_diagnostics.csv"),
        )
    shutil.rmtree(work_dir, ignore_errors=True)
    return {
        "valid": not reasons,
        "reason": "; ".join(reasons) if reasons else "aprovada",
        "primary_failure": primary_failure or "",
        "offset_n": offset_n,
        "cycles": useful_cycles,
        "warmup_cycles": warmup_cycles,
        "executed_cycles": executed_cycles,
        "strokes": useful_strokes,
        "strokes_executed": target_strokes,
        "cycle_means": cycle_means,
        "rpm_drive": rpm_drive,
        "rpm_disk_target": rpm_disk_target,
        "rpm_disk_effective": rpm_disk_effective,
        "valid_samples": valid_count,
        "total_samples": total_rows,
        "loss_pct": loss_pct,
        "theoretical_s": theoretical_s,
        "capture_csv": capture_csv,
        "encoder_forward_sign": int(shadow_summary.get("forward_sign", "0") or 0),
        "encoder_shadow_summary": shadow_summary,
        "encoder_capture_quality": encoder_capture_quality,
        "encoder_stop_diagnostics": stop_diagnostics,
    }


def wait_and_merge(state: RunState) -> int:
    """
    Aguarda termino dos dois loggers e executa o merge.

    Fluxo:
    1) Espera DLG encerrar.
    2) Espera Drive encerrar.
    3) Tenta merge em C (merge_logs.exe).
    4) Se merge em C falhar ou nao gerar arquivo, aplica alternativa em Python.

    Parametros:
        state: estado retornado por start_external_run().

    Retorna:
        Codigo de retorno do merge.
        - Quando alternativa Python e usado com sucesso, retorna 0.
    """
    if state.reciprocating:
        # A distancia encerra o movimento, mas o Drive permanece parado e os
        # dois loggers continuam por uma curta pos-captura. Isso permite que a
        # cauda atrasada do DLG (CH1 + CH3) chegue antes do fechamento.
        event_path = os.path.join(os.path.dirname(state.drive_csv), "a5_speed_events.log")
        event_pos = 0
        motion_done_at = None
        dlg_pos = 0
        angle_last = None
        angle_unwrapped = None
        post_angles = []
        stop_sent_after_settle = False

        def append_post_lines(lines, done_qpc=None):
            nonlocal angle_last, angle_unwrapped
            parsed = []
            for line in lines:
                cols = line.strip().split(",")
                if len(cols) <= 5 or cols[5].strip().upper() == "NULL":
                    continue
                try:
                    parsed.append((
                        int(cols[0]), int(cols[1]), float(cols[2]), float(cols[5])
                    ))
                except Exception:
                    continue
            if done_qpc is not None and parsed and not post_angles:
                start = min(
                    range(len(parsed)),
                    key=lambda pos: abs(parsed[pos][1] - int(done_qpc)),
                )
                parsed = parsed[start:]
            for sample_idx, _sample_qpc, sample_t_s, angle in parsed:
                if angle_last is None:
                    angle_unwrapped = angle
                else:
                    delta = angle - angle_last
                    if delta > 180.0:
                        delta -= 360.0
                    elif delta < -180.0:
                        delta += 360.0
                    angle_unwrapped += delta
                angle_last = angle
                post_angles.append((sample_idx, sample_t_s, angle_unwrapped))

        while state.dlg_proc.poll() is None and state.drive_proc.poll() is None:
            if os.path.exists(event_path):
                try:
                    with open(event_path, "r", encoding="ascii", errors="replace") as fev:
                        fev.seek(event_pos)
                        chunk = fev.read()
                        event_pos = fev.tell()
                    if motion_done_at is None and (
                        "RECIP_ENCODER_DONE " in chunk or "RECIP_DONE " in chunk
                    ):
                        motion_done_at = time.monotonic()
                        done_qpc = None
                        for event_line in chunk.splitlines():
                            if "RECIP_ENCODER_DONE " in event_line or "RECIP_DONE " in event_line:
                                fields = _event_fields(event_line)
                                try:
                                    done_qpc = int(fields.get("qpc", ""))
                                except Exception:
                                    done_qpc = None
                        if os.path.exists(state.dlg_csv):
                            with open(state.dlg_csv, "r", encoding="ascii", errors="replace") as fdlg:
                                existing = fdlg.readlines()
                                dlg_pos = fdlg.tell()
                            append_post_lines(existing, done_qpc=done_qpc)
                except Exception:
                    pass

            if motion_done_at is not None and os.path.exists(state.dlg_csv):
                try:
                    with open(state.dlg_csv, "r", encoding="ascii", errors="replace") as fdlg:
                        fdlg.seek(dlg_pos)
                        lines = fdlg.readlines()
                        dlg_pos = fdlg.tell()
                    append_post_lines(lines)
                except Exception:
                    pass

                elapsed = time.monotonic() - motion_done_at
                endpoint = _detect_reciprocating_stationary_endpoint(
                    post_angles, float(state.raio_mm)
                )
                if elapsed >= RECIP_FINAL_MIN_OBSERVATION_S and endpoint is not None:
                    state.reciprocating_final_encoder_idx = endpoint[0]
                    state.reciprocating_final_encoder_t_s = endpoint[1]
                    state.reciprocating_postroll_status = "encoder_estavel"
                    _send_ipc(state.dlg_proc, "STOP")
                    _send_ipc(state.drive_proc, "STOP")
                    stop_sent_after_settle = True
                    break
                if elapsed >= 1.8:
                    state.reciprocating_postroll_status = "timeout"
                    _send_ipc(state.dlg_proc, "STOP")
                    _send_ipc(state.drive_proc, "STOP")
                    stop_sent_after_settle = True
                    break
            time.sleep(0.05)
        if not stop_sent_after_settle and state.drive_proc.poll() is not None and state.dlg_proc.poll() is None:
            _send_ipc(state.dlg_proc, "STOP")
        elif not stop_sent_after_settle and state.dlg_proc.poll() is not None and state.drive_proc.poll() is None:
            _send_ipc(state.drive_proc, "STOP")
        state.dlg_proc.wait()
        state.drive_proc.wait()
    elif state.continuous_target_mm > 0.0:
        while state.dlg_proc.poll() is None and state.drive_proc.poll() is None:
            time.sleep(0.02)
        if state.dlg_proc.poll() is None:
            _send_ipc(state.dlg_proc, "STOP")
        if state.drive_proc.poll() is None:
            _send_ipc(state.drive_proc, "STOP")
        state.dlg_proc.wait()
        state.drive_proc.wait()
    else:
        state.dlg_proc.wait()
        state.drive_proc.wait()
    dlg_rc = state.dlg_proc.returncode
    drive_rc = state.drive_proc.returncode
    if (
        state.reciprocating_shadow_enabled or
        state.reciprocating_encoder_control_enabled
    ):
        load_reciprocating_shadow_summary(state)
        build_reciprocating_stop_diagnostics(state)

    if state.reciprocating and state.reciprocating_encoder_control_enabled:
        _rebuild_reciprocating_csv_from_logs(
            state,
            float(state.relacao),
            float(state.raio_mm),
            float(state.distance_interval_mm),
        )
        _move_dev_artifacts(state)
        if drive_rc not in (None, 0):
            return int(drive_rc)
        if dlg_rc not in (None, 0):
            return int(dlg_rc)
        return 0

    if not state.reciprocating and state.continuous_target_mm > 0.0:
        if state.encoder_monitor_thread and state.encoder_monitor_thread.is_alive():
            state.encoder_monitor_thread.join(timeout=0.5)
        _build_continuous_encoder_outputs(state)
        _move_dev_artifacts(state)
        if drive_rc not in (None, 0):
            return int(drive_rc)
        if dlg_rc not in (None, 0):
            return int(dlg_rc)
        if state.continuous_encoder_status not in ("alvo_atingido", "parada_manual"):
            return 4
        return 0

    # Reprocessa sempre no final para garantir consistencia do arquivo final
    # com os CSVs completos (independente de glitch no tempo real).
    _rebuild_turn_csv_from_logs(state)

    merge_rc = -1
    processing_dlg_csv = _processing_dlg_csv(state)
    merge_cmd = [
        state.merge_exe,
        "--dlg", processing_dlg_csv,
        "--drive", state.drive_csv,
        "--out", state.merge_csv,
    ]
    try:
        merge_rc = subprocess.call(merge_cmd)
    except Exception:
        merge_rc = -1

    # Rede de seguranca: se merge em C falhar, gera resultado em Python.
    if merge_rc != 0 or not os.path.exists(state.merge_csv):
        _merge_csv_fallback(processing_dlg_csv, state.drive_csv, state.merge_csv)
        _append_external_encoder_position(state)
        _apply_dynamic_offset_to_merge(state)
        _crop_reciprocating_merge_at_encoder_endpoint(state)
        _move_dev_artifacts(state)
        if drive_rc not in (None, 0):
            return int(drive_rc)
        if dlg_rc not in (None, 0):
            return int(dlg_rc)
        return 0

    _append_external_encoder_position(state)
    _apply_dynamic_offset_to_merge(state)
    _crop_reciprocating_merge_at_encoder_endpoint(state)
    _move_dev_artifacts(state)
    if drive_rc not in (None, 0):
        return int(drive_rc)
    if dlg_rc not in (None, 0):
        return int(dlg_rc)
    return merge_rc


def _nearest_unwrapped_angle(angle_deg: float, reference_deg: float) -> float:
    """Retorna a representacao de angle_deg mais proxima da referencia."""
    return angle_deg + 360.0 * round((reference_deg - angle_deg) / 360.0)


def _median(values: List[float]) -> float:
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[middle]
    return 0.5 * (ordered[middle - 1] + ordered[middle])


def _filter_external_encoder_angles(
    raw_angles: List[Optional[float]],
    nominal_step_deg: float,
    sample_rate_hz: float = DEFAULT_DRIVE_RATE_HZ,
) -> Tuple[List[Optional[float]], List[int]]:
    """Rastreador angular com innovation gate, histerese e quarentena.

    Amostras coerentes passam sem suavizacao. Quando a inovacao excede o
    limite dinamico, o rastreador entra em quarentena e exige uma sequencia
    novamente coerente antes de liberar o sinal. O trecho isolado e
    reconstruido linearmente entre as duas ancoras confiaveis.
    """
    count = len(raw_angles)
    output: List[Optional[float]] = [None] * count
    quarantine = [0] * count
    nominal_step = max(0.0, float(nominal_step_deg))
    rate_hz = max(1.0, float(sample_rate_hz))
    confirm_samples = max(2, int(round(ENCODER_TRANSITION_CONFIRM_S * rate_hz)))
    velocity_history_samples = max(
        2, int(round(ENCODER_TRANSITION_VELOCITY_HISTORY_S * rate_hz))
    )
    local_gate = max(
        ENCODER_TRANSITION_MIN_GATE_DEG,
        nominal_step * 3.0 + 0.75,
    )

    last_angle: Optional[float] = None
    last_idx: Optional[int] = None
    velocity = 0.0
    velocity_history: List[float] = []
    in_quarantine = False
    buffered: List[Tuple[int, float]] = []
    stable_chain: List[Tuple[int, float]] = []

    def remember_step(step: float) -> None:
        nonlocal velocity, velocity_history
        velocity_history.append(step)
        velocity_history = velocity_history[-velocity_history_samples:]
        velocity = _median(velocity_history)

    def close_quarantine(endpoint_idx: int, endpoint_angle: float) -> None:
        nonlocal last_angle, last_idx, buffered, stable_chain, in_quarantine
        if last_angle is None or last_idx is None:
            return
        span = max(1, endpoint_idx - last_idx)
        for sample_idx, _raw in buffered:
            fraction = (sample_idx - last_idx) / span
            output[sample_idx] = last_angle + (endpoint_angle - last_angle) * fraction
            quarantine[sample_idx] = 1
        remember_step((endpoint_angle - last_angle) / span)
        last_angle = endpoint_angle
        last_idx = endpoint_idx
        buffered = []
        stable_chain = []
        in_quarantine = False

    for idx, raw_angle in enumerate(raw_angles):
        if raw_angle is None or not math.isfinite(raw_angle):
            if in_quarantine:
                quarantine[idx] = 1
            continue
        normalized = raw_angle % 360.0
        if last_angle is None or last_idx is None:
            output[idx] = normalized
            last_angle = normalized
            last_idx = idx
            continue

        elapsed = max(1, idx - last_idx)
        prediction = last_angle + velocity * elapsed
        if not in_quarantine:
            candidate = _nearest_unwrapped_angle(normalized, prediction)
            innovation_gate = max(local_gate, abs(velocity) * 3.0 + 0.75)
            if abs(candidate - prediction) <= innovation_gate:
                output[idx] = candidate
                remember_step((candidate - last_angle) / elapsed)
                last_angle = candidate
                last_idx = idx
                continue
            in_quarantine = True
            buffered = [(idx, normalized)]
            stable_chain = [(idx, candidate)]
            quarantine[idx] = 1
            continue

        buffered.append((idx, normalized))
        quarantine[idx] = 1
        previous_reference = stable_chain[-1][1] if stable_chain else prediction
        candidate = _nearest_unwrapped_angle(normalized, previous_reference)
        if abs(candidate - previous_reference) <= local_gate:
            stable_chain.append((idx, candidate))
        else:
            stable_chain = [(idx, _nearest_unwrapped_angle(normalized, prediction))]

        if len(stable_chain) >= confirm_samples:
            endpoint_idx, endpoint_angle = stable_chain[-1]
            elapsed = max(1, endpoint_idx - last_idx)
            prediction = last_angle + velocity * elapsed
            recovery_gate = max(
                8.0,
                local_gate + elapsed * max(abs(velocity), nominal_step) * 1.5,
            )
            if abs(endpoint_angle - prediction) <= recovery_gate:
                close_quarantine(endpoint_idx, endpoint_angle)

    # Se a captura terminar durante uma transicao, mantem a previsao causal e
    # marca todo o trecho como quarentena; nunca publica o valor bruto instavel.
    if in_quarantine and last_angle is not None and last_idx is not None:
        for sample_idx, _raw in buffered:
            output[sample_idx] = last_angle + velocity * (sample_idx - last_idx)
            quarantine[sample_idx] = 1

    normalized_output = [
        None if angle is None else angle % 360.0
        for angle in output
    ]
    return normalized_output, quarantine


def _encoder_nominal_step_deg(state: RunState) -> float:
    """Maior passo angular esperado por amostra, a partir do ensaio."""
    rate_hz = _processing_rate_hz(state)
    radius_mm = float(getattr(state, "raio_mm", 0.0) or 0.0)
    schedule = getattr(state, "target_speed_schedule", None) or []
    speeds = [abs(float(item[0])) for item in schedule if item]
    if rate_hz > 0.0 and radius_mm > 0.0 and speeds:
        return max(speeds) * 180.0 / (math.pi * radius_mm * rate_hz)
    return 1.0


def _append_external_encoder_position(state: RunState) -> None:
    """Acrescenta PosEncExt filtrada e sua marca de quarentena."""
    if not os.path.exists(state.merge_csv):
        return
    tmp_path = state.merge_csv + ".encoder_tmp"
    with open(state.merge_csv, "r", newline="", encoding="utf-8") as src, \
         open(tmp_path, "w", newline="", encoding="utf-8") as dst:
        reader = csv.reader(src, delimiter=";")
        writer = csv.writer(dst, delimiter=";", lineterminator="\n")
        header = next(reader, None)
        if not header:
            writer.writerow([])
        else:
            names = {name.strip().lower(): i for i, name in enumerate(header)}
            if "posencext" in names and "posencext_quarentena" in names:
                writer.writerow(header)
                writer.writerows(reader)
            else:
                rows = list(reader)
                ch3_i = names.get("ch3")
                raw_angles: List[Optional[float]] = []
                for row in rows:
                    angle: Optional[float] = None
                    if ch3_i is not None and ch3_i < len(row):
                        text_value = row[ch3_i].strip()
                        if text_value and text_value.upper() != "NULL":
                            try:
                                angle = float(text_value) % 360.0
                            except Exception:
                                angle = None
                    raw_angles.append(angle)
                filtered, quarantine = _filter_external_encoder_angles(
                    raw_angles,
                    _encoder_nominal_step_deg(state),
                    _processing_rate_hz(state),
                )
                state.encoder_quarantine_samples = sum(quarantine)
                state.encoder_quarantine_fraction = (
                    state.encoder_quarantine_samples / len(quarantine)
                    if quarantine else 0.0
                )
                base_header = [
                    name for name in header
                    if name.strip().lower() not in ("posencext", "posencext_quarentena")
                ]
                writer.writerow([*base_header, "PosEncExt", "PosEncExt_Quarentena"])
                old_pos_i = names.get("posencext")
                old_quarantine_i = names.get("posencext_quarentena")
                remove_indices = {i for i in (old_pos_i, old_quarantine_i) if i is not None}
                for row, angle, isolated in zip(rows, filtered, quarantine):
                    base_row = [value for i, value in enumerate(row) if i not in remove_indices]
                    value = "NULL" if angle is None else f"{angle:.10g}"
                    writer.writerow([*base_row, value, str(int(isolated))])
    os.replace(tmp_path, state.merge_csv)


def _crop_reciprocating_merge_at_encoder_endpoint(state: RunState) -> None:
    """Remove apenas a cauda estatica final; DadosDev preserva a pos-captura completa."""
    if not state.reciprocating or not os.path.isfile(state.merge_csv):
        return
    endpoint_t_s = getattr(state, "reciprocating_final_encoder_t_s", None)
    endpoint_idx = getattr(state, "reciprocating_final_encoder_idx", None)
    if endpoint_t_s is None and endpoint_idx is None:
        return
    tmp_path = state.merge_csv + ".recip_crop_tmp"
    try:
        with open(state.merge_csv, "r", newline="", encoding="utf-8") as src, \
             open(tmp_path, "w", newline="", encoding="utf-8") as dst:
            reader = csv.reader(src, delimiter=";")
            writer = csv.writer(dst, delimiter=";", lineterminator="\n")
            header = next(reader, None)
            if header:
                writer.writerow(header)
            names = {
                name.strip().lower(): i for i, name in enumerate(header or [])
            }
            t_s_i = names.get("t_s", 1)
            for row in reader:
                try:
                    beyond_endpoint = (
                        float(row[t_s_i]) > float(endpoint_t_s) + 1.0e-9
                        if endpoint_t_s is not None
                        else int(row[0]) > int(endpoint_idx)
                    )
                except Exception:
                    continue
                if beyond_endpoint:
                    break
                writer.writerow(row)
        os.replace(tmp_path, state.merge_csv)
    finally:
        if os.path.exists(tmp_path):
            try:
                os.remove(tmp_path)
            except Exception:
                pass


def _apply_dynamic_offset_to_merge(state: RunState) -> None:
    """Corrige CH1 e atrito no _T final, preservando o dlg.csv tecnico bruto."""
    offset = float(getattr(state, "dynamic_offset_n", 0.0) or 0.0)
    if not state.reciprocating or not os.path.exists(state.merge_csv):
        return
    tmp_path = state.merge_csv + ".offset_tmp"
    with open(state.merge_csv, "r", newline="", encoding="utf-8") as src, \
         open(tmp_path, "w", newline="", encoding="utf-8") as dst:
        reader = csv.reader(src, delimiter=";")
        writer = csv.writer(dst, delimiter=";", lineterminator="\n")
        header = next(reader, None)
        if not header:
            writer.writerow([])
        else:
            writer.writerow(header)
            names = {name.strip().lower(): i for i, name in enumerate(header)}
            ch1_i = names.get("ch1", 2)
            atr_i = names.get("atrito", 10)
            for row in reader:
                try:
                    if ch1_i < len(row) and row[ch1_i].strip().upper() != "NULL":
                        corrected = float(row[ch1_i]) - offset
                        row[ch1_i] = f"{corrected:.10g}"
                        if atr_i < len(row):
                            if state.force_normal_n > 0.0:
                                row[atr_i] = f"{corrected / state.force_normal_n:.10g}"
                            else:
                                row[atr_i] = "NULL"
                except Exception:
                    pass
                writer.writerow(row)
    os.replace(tmp_path, state.merge_csv)


def _move_dev_artifacts(state: RunState) -> None:
    """
    Move artefatos tecnicos do ensaio para a pasta DadosDev.

    Motivacao:
    - Manter a pasta REP focada nos arquivos finais para usuario.
    - Preservar logs tecnicos para diagnostico sem mistura visual.
    """
    if not state:
        return
    rep_dir = os.path.dirname(state.merge_csv) if state.merge_csv else ""
    if not rep_dir:
        return

    dev_dir = os.path.join(rep_dir, "DadosDev")
    os.makedirs(dev_dir, exist_ok=True)

    pairs = []
    merge_base = os.path.basename(state.merge_csv) if state.merge_csv else "resultado_ensaio.csv"
    merge_source_name = f"{merge_base}.merge_source"
    # Signature do merge em C: suporte a nome legado (.txt) e novo.
    merge_sig_new = f"{state.merge_csv}.merge_source"
    merge_sig_old = f"{state.merge_csv}.merge_source.txt"
    if os.path.exists(merge_sig_new):
        pairs.append((merge_sig_new, os.path.join(dev_dir, merge_source_name)))
    elif os.path.exists(merge_sig_old):
        pairs.append((merge_sig_old, os.path.join(dev_dir, merge_source_name)))

    pairs.extend([
        (state.dlg_csv, os.path.join(dev_dir, "dlg.csv")),
        (state.drive_csv, os.path.join(dev_dir, "drive.csv")),
        (state.schedule_csv, os.path.join(dev_dir, "schedule.csv")),
        (os.path.join(rep_dir, "graph_events.log"), os.path.join(dev_dir, "graph_events.log")),
        (os.path.join(rep_dir, "dlg_logger_events.log"), os.path.join(dev_dir, "dlg_logger_events.log")),
        (os.path.join(rep_dir, "a5_speed_events.log"), os.path.join(dev_dir, "a5_speed_events.log")),
        (os.path.join(rep_dir, "recip_stop_diagnostics.csv"), os.path.join(dev_dir, "recip_stop_diagnostics.csv")),
    ])
    if state.dlg_compat_csv and state.dlg_compat_csv != state.dlg_csv:
        pairs.append((state.dlg_compat_csv, os.path.join(dev_dir, "dlg_compat_50hz.csv")))
    if state.encoder_state_csv:
        pairs.append((state.encoder_state_csv, os.path.join(dev_dir, "encoder_state.csv")))

    for src, dst in pairs:
        _move_file_best_effort(src, dst)


def _move_file_best_effort(src: str, dst: str) -> None:
    """
    Move arquivo com tolerancia a locks temporarios no Windows.

    Observacao:
    - shutil.move pode fazer copy+delete em alguns cenarios.
      Se o delete falhar por lock, ficamos com copia no destino e origem
      ainda presente. Neste caso tentamos remover a origem explicitamente.
    """
    try:
        if not src or not os.path.exists(src):
            return
        if os.path.abspath(src) == os.path.abspath(dst):
            return
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.exists(dst):
            os.remove(dst)
        try:
            # rename atomico quando possivel (mesmo volume)
            os.replace(src, dst)
        except Exception:
            shutil.move(src, dst)

        # Se sobrar arquivo na origem (copy+delete parcial), tenta limpar.
        if os.path.exists(src) and os.path.exists(dst):
            try:
                os.remove(src)
            except Exception:
                pass
    except Exception:
        # Melhor esforco: nao falha ensaio por erro de organizacao de arquivos.
        pass


def finalize_dev_artifacts_after_run(state: RunState, retries: int = 20, sleep_s: float = 0.1) -> None:
    """
    Segunda passada de limpeza dos CSVs tecnicos apos fim do ensaio.

    Uso:
    - Chamar depois que a UI marcar ensaio como encerrado (running=False),
      para reduzir chance de lock por threads de tail ainda abertas.
    """
    if not state:
        return
    rep_dir = os.path.dirname(state.merge_csv) if state.merge_csv else ""
    if not rep_dir:
        return
    dev_dir = os.path.join(rep_dir, "DadosDev")
    os.makedirs(dev_dir, exist_ok=True)

    pending = [
        (state.dlg_csv, os.path.join(dev_dir, "dlg.csv")),
        (state.drive_csv, os.path.join(dev_dir, "drive.csv")),
    ]
    if state.dlg_compat_csv and state.dlg_compat_csv != state.dlg_csv:
        pending.append((state.dlg_compat_csv, os.path.join(dev_dir, "dlg_compat_50hz.csv")))
    if state.encoder_state_csv:
        pending.append((state.encoder_state_csv, os.path.join(dev_dir, "encoder_state.csv")))

    for _ in range(max(1, retries)):
        all_done = True
        for src, dst in pending:
            if src and os.path.exists(src):
                _move_file_best_effort(src, dst)
            if src and os.path.exists(src):
                all_done = False
        if all_done:
            break
        time.sleep(max(0.01, float(sleep_s)))


def _rebuild_reciprocating_csv_from_drive_legacy(state: RunState, relacao: float, raio_mm: float, distance_interval_mm: float) -> None:
    """
    Regera _DP e _M para modo reciprocante usando deslocamento absoluto do encoder.

    O _M usa strokes delimitados pelos eventos RECIP_REVERSE/RECIP_DONE do
    a5_speed_events.log. Isso evita depender do RPM para decidir sentido nas
    inversoes, onde a telemetria pode ficar nula ou atrasada.
    """
    def _to_int(txt, default=None):
        try:
            if txt is None:
                return default
            t = str(txt).strip()
            if not t or t.upper() == "NULL":
                return default
            return int(t)
        except Exception:
            return default

    def _to_float(txt, default=None):
        try:
            if txt is None:
                return default
            t = str(txt).strip()
            if not t or t.upper() == "NULL":
                return default
            return float(t)
        except Exception:
            return default

    def _fmt(value, digits=6):
        if value is None:
            return "NULL"
        try:
            return f"{float(value):.{digits}f}"
        except Exception:
            return "NULL"

    def _event_kv(line):
        out = {}
        for part in str(line).replace(",", " ").split():
            if "=" not in part:
                continue
            k, v = part.split("=", 1)
            out[k.strip()] = v.strip()
        return out

    def _parse_recip_events(path):
        info = {
            "pos_mod": 65536.0,
            "reverses": [],
            "reverse_error_counts": {},
            "boundary_segments": {},
            "done": None,
            "done_error_counts": None,
        }
        if not path or not os.path.exists(path):
            return info
        try:
            with open(path, "r", encoding="utf-8") as f:
                for line in f:
                    if "RECIP_INIT " in line:
                        kv = _event_kv(line)
                        pos_mod = _to_float(kv.get("pos_mod"), None)
                        if pos_mod is not None and pos_mod > 1.0:
                            info["pos_mod"] = pos_mod
                    elif "RECIP_REVERSE " in line:
                        kv = _event_kv(line)
                        idx = _to_int(kv.get("idx"), None)
                        if idx is not None:
                            info["reverses"].append(idx)
                            error_counts = _to_float(kv.get("error_counts"), None)
                            if error_counts is not None:
                                info["reverse_error_counts"][idx] = error_counts
                            segment = _to_int(kv.get("completed_segment"), None)
                            if segment is not None:
                                info["boundary_segments"][idx] = segment
                    elif "RECIP_DONE " in line:
                        kv = _event_kv(line)
                        idx = _to_int(kv.get("idx"), None)
                        if idx is not None:
                            info["done"] = idx
                            info["done_error_counts"] = _to_float(
                                kv.get("error_counts"), None
                            )
                            segment = _to_int(kv.get("completed_segment"), None)
                            if segment is not None:
                                info["boundary_segments"][idx] = segment
        except Exception:
            return info
        info["reverses"] = sorted(set(info["reverses"]))
        return info

    def _read_aligned_samples():
        samples = []
        with open(_processing_dlg_csv(state), "r", newline="", encoding="utf-8") as fdlg, \
             open(state.drive_csv, "r", newline="", encoding="utf-8") as fdrv:
            rd_d = csv.reader(fdlg)
            rd_r = csv.reader(fdrv)
            next(rd_d, None)
            next(rd_r, None)

            drow = None
            rrow = None
            have_d = False
            have_r = False
            while True:
                if not have_d:
                    try:
                        drow = next(rd_d)
                        have_d = True
                    except StopIteration:
                        break
                if not have_r:
                    try:
                        rrow = next(rd_r)
                        have_r = True
                    except StopIteration:
                        break

                idx_d = _to_int(drow[0] if drow and len(drow) > 0 else None, None)
                idx_r = _to_int(rrow[0] if rrow and len(rrow) > 0 else None, None)
                if idx_d is None:
                    have_d = False
                    continue
                if idx_r is None:
                    have_r = False
                    continue
                if idx_d < idx_r:
                    have_d = False
                    continue
                if idx_r < idx_d:
                    have_r = False
                    continue

                have_d = False
                have_r = False

                dlg_err = _to_int(drow[-1] if drow else None, 1)
                ch1 = _to_float(drow[3] if drow and len(drow) > 3 else None, None)
                atr = _to_float(drow[11] if drow and len(drow) > 11 else None, None)
                if ch1 is not None:
                    ch1 -= float(getattr(state, "dynamic_offset_n", 0.0) or 0.0)
                if state.force_normal_n and state.force_normal_n > 0 and ch1 is not None:
                    atr = ch1 / state.force_normal_n
                atr_ok = (dlg_err == 0 and atr is not None)

                t_s_dlg = _to_float(drow[2] if drow and len(drow) > 2 else None, None)
                t_s_drv = _to_float(rrow[2] if rrow and len(rrow) > 2 else None, None)
                t_s = t_s_drv if t_s_drv is not None else t_s_dlg

                pos_err = _to_int(rrow[5] if rrow and len(rrow) > 5 else None, 1)
                rpm_err = _to_int(rrow[6] if rrow and len(rrow) > 6 else None, 1)
                pos = _to_float(rrow[3] if rrow and len(rrow) > 3 else None, None)
                rpm = _to_float(rrow[4] if rrow and len(rrow) > 4 else None, None)
                pos_mod = _to_float(rrow[7] if rrow and len(rrow) > 7 else None, None)
                if pos_mod is None or pos_mod <= 1.0:
                    pos_mod = None

                samples.append({
                    "idx": idx_d,
                    "t": t_s,
                    "atr": atr,
                    "atr_ok": atr_ok,
                    "pos": pos,
                    "pos_ok": (pos_err == 0 and pos is not None),
                    "rpm": rpm,
                    "rpm_ok": (rpm_err == 0 and rpm is not None),
                    "pos_mod": pos_mod,
                })
        return samples

    def _delta_mm(prev_s, cur_s, fallback_mod):
        if not prev_s or not cur_s:
            return 0.0
        mod = cur_s.get("pos_mod") or prev_s.get("pos_mod") or fallback_mod
        if mod is None or mod <= 1.0 or relacao <= 0.0 or raio_mm <= 0.0:
            return 0.0
        raw = abs(float(cur_s["pos"]) - float(prev_s["pos"]))
        if raw > (0.5 * mod):
            raw = mod - raw
        if raw < 0.0:
            raw = 0.0
        return raw * (2.0 * 3.141592653589793 * raio_mm) / (relacao * mod)

    def _dp_acc_reset():
        return {
            "n_total": 0,
            "n_fail": 0,
            "n_valid": 0,
            "t_start_s": None,
            "t_end_s": None,
            "atr_sum": 0.0,
            "atr_min": None,
            "atr_max": None,
            "rpm_sum": 0.0,
            "rpm_cnt": 0,
        }

    def _dp_add(acc, s):
        acc["n_total"] += 1
        if acc["t_start_s"] is None:
            acc["t_start_s"] = s.get("t")
        if s.get("t") is not None:
            acc["t_end_s"] = s.get("t")
        if s.get("atr_ok") and s.get("pos_ok"):
            atr = s["atr"]
            acc["n_valid"] += 1
            # No reciprocante, sentidos opostos nao podem se cancelar na
            # media por distancia. Minimo e maximo continuam assinados.
            acc["atr_sum"] += abs(atr)
            acc["atr_min"] = atr if acc["atr_min"] is None else min(acc["atr_min"], atr)
            acc["atr_max"] = atr if acc["atr_max"] is None else max(acc["atr_max"], atr)
            if s.get("rpm_ok"):
                acc["rpm_sum"] += abs(s["rpm"])
                acc["rpm_cnt"] += 1
        else:
            acc["n_fail"] += 1

    def _emit_dp(writer, boundary_mm, acc):
        pct_loss = (100.0 * acc["n_fail"] / acc["n_total"]) if acc["n_total"] > 0 else 0.0
        atr_med = (acc["atr_sum"] / acc["n_valid"]) if acc["n_valid"] > 0 else None
        rpm_med = (acc["rpm_sum"] / acc["rpm_cnt"]) if acc["rpm_cnt"] > 0 else None
        vel_med = None
        if acc["t_start_s"] is not None and acc["t_end_s"] is not None:
            dt = acc["t_end_s"] - acc["t_start_s"]
            if dt > 0.0:
                vel_med = distance_interval_mm / dt
        writer.writerow([
            _fmt(boundary_mm),
            _fmt(acc["t_start_s"]),
            _fmt(atr_med),
            _fmt(acc["atr_min"]),
            _fmt(acc["atr_max"]),
            _fmt(rpm_med),
            _fmt(vel_med),
            acc["n_total"], acc["n_fail"], acc["n_valid"], f"{pct_loss:.3f}"
        ])

    def _emit_motion(writer, stroke_n, stroke_samples, edge_pct, fallback_mod,
                     event_error_mm=None, target_speed_mm_s=None):
        first_t = None
        last_t = None
        prev_pos_sample = None
        stroke_len_mm = 0.0
        values = []

        for s in stroke_samples:
            if first_t is None and s.get("t") is not None:
                first_t = s.get("t")
            if s.get("t") is not None:
                last_t = s.get("t")
            if not s.get("pos_ok"):
                continue
            if prev_pos_sample is not None:
                stroke_len_mm += _delta_mm(prev_pos_sample, s, fallback_mod)
            if s.get("atr_ok"):
                values.append((stroke_len_mm, s["atr"]))
            prev_pos_sample = s

        edge_mm = (stroke_len_mm * edge_pct / 100.0) if edge_pct > 0.0 else 0.0
        if edge_pct <= 0.0:
            filtered = values
        else:
            filtered = [
                item for item in values
                if item[0] >= edge_mm and item[0] <= (stroke_len_mm - edge_mm)
            ]

        atr_rms = None
        atr_med = None
        atr_max = None
        atr_min = None
        pos_max = None
        pos_min = None
        if filtered:
            atr_values = [item[1] for item in filtered]
            atr_rms = (sum(v * v for v in atr_values) / float(len(atr_values))) ** 0.5
            atr_med = sum(abs(v) for v in atr_values) / float(len(atr_values))
            max_item = max(filtered, key=lambda item: item[1])
            min_item = min(filtered, key=lambda item: item[1])
            atr_max = max_item[1]
            pos_max = max_item[0]
            atr_min = min_item[1]
            pos_min = min_item[0]

        vel_mm_s = None
        if first_t is not None and last_t is not None:
            dt = last_t - first_t
            if dt > 0.0:
                vel_mm_s = stroke_len_mm / dt

        tempo_min = (first_t / 60.0) if first_t is not None else None
        course_error_mm = event_error_mm
        if course_error_mm is None:
            course_error_mm = stroke_len_mm - float(state.reciprocating_course_mm)
        writer.writerow([
            _fmt(tempo_min),
            stroke_n,
            _fmt(atr_rms),
            _fmt(atr_med),
            _fmt(atr_max),
            _fmt(pos_max),
            _fmt(atr_min),
            _fmt(pos_min),
            len(filtered),
            _fmt(vel_mm_s),
            _fmt(target_speed_mm_s),
            _fmt(course_error_mm),
        ])

    if relacao <= 0.0 or raio_mm <= 0.0:
        return
    try:
        edge_pct = float(getattr(state, "reciprocating_edge_filter_pct", 0.0))
    except Exception:
        edge_pct = 0.0
    if edge_pct < 0.0:
        edge_pct = 0.0
    if edge_pct > 100.0:
        edge_pct = 100.0
    if distance_interval_mm <= 0.0:
        distance_interval_mm = 10.0

    events_path = os.path.join(os.path.dirname(state.drive_csv), "a5_speed_events.log")
    events = _parse_recip_events(events_path)
    samples = _read_aligned_samples()
    fallback_mod = events.get("pos_mod") or 65536.0

    with open(state.turn_dist_csv, "w", newline="", encoding="utf-8") as fdist, \
         open(state.turn_vp_csv, "w", newline="", encoding="utf-8") as fmotion:
        wd = csv.writer(fdist, delimiter=";", lineterminator="\n")
        wm = csv.writer(fmotion, delimiter=";", lineterminator="\n")
        wd.writerow([
            "distancia_mm", "t_s_inicio", "atrito_med", "atrito_min", "atrito_max", "rpm_medio_intervalo", "velocidade_media_mm_s",
            "n_total_pontos", "n_falhas", "n_validas", "pct_perda"
        ])
        wm.writerow([
            "TEMPO_(min)", "STROKE", "ATRITO_EFETIVO", "ATRITO_MEDIO", "ATRITO_MAX", "POS_MAX", "ATRITO_MIN", "POS_MIN", "LINHAS", "VELOCIDADE_MEDIA", "VELOCIDADE_ALVO", "ERRO_CURSO_MM"
        ])
        if not samples:
            return

        acc = _dp_acc_reset()
        prev_pos_sample = None
        cum_dist_mm = 0.0
        next_boundary_mm = distance_interval_mm
        for s in samples:
            _dp_add(acc, s)
            if s.get("pos_ok") and prev_pos_sample is not None:
                cum_dist_mm += _delta_mm(prev_pos_sample, s, fallback_mod)
                while cum_dist_mm >= next_boundary_mm:
                    _emit_dp(wd, next_boundary_mm, acc)
                    next_boundary_mm += distance_interval_mm
                    acc = _dp_acc_reset()
            if s.get("pos_ok"):
                prev_pos_sample = s

        boundaries = list(events.get("reverses") or [])
        done_idx = events.get("done")
        if done_idx is not None and (not boundaries or done_idx > boundaries[-1]):
            boundaries.append(done_idx)
        if not boundaries:
            boundaries.append(samples[-1]["idx"])

        stroke_n = 1
        start_idx = samples[0]["idx"]
        cursor = 0
        for boundary_idx in boundaries:
            if boundary_idx < start_idx:
                continue
            while cursor < len(samples) and samples[cursor]["idx"] < start_idx:
                cursor += 1
            start_cursor = cursor
            while cursor < len(samples) and samples[cursor]["idx"] <= boundary_idx:
                cursor += 1
            stroke_samples = samples[start_cursor:cursor]
            if stroke_samples:
                error_counts = events.get("reverse_error_counts", {}).get(boundary_idx)
                if error_counts is None and done_idx == boundary_idx:
                    error_counts = events.get("done_error_counts")
                event_error_mm = None
                if error_counts is not None and fallback_mod > 1.0:
                    event_error_mm = (
                        error_counts * (2.0 * 3.141592653589793 * raio_mm)
                        / (relacao * fallback_mod)
                    )
                target_speed = None
                segment = events.get("boundary_segments", {}).get(boundary_idx)
                target_schedule = list(getattr(state, "target_speed_schedule", None) or [])
                if segment is not None and 0 <= segment < len(target_schedule):
                    try:
                        target_speed = float(target_schedule[segment][0])
                    except Exception:
                        target_speed = None
                _emit_motion(
                    wm, stroke_n, stroke_samples, edge_pct, fallback_mod,
                    event_error_mm=event_error_mm,
                    target_speed_mm_s=target_speed,
                )
                stroke_n += 1
            start_idx = boundary_idx + 1


def _rebuild_reciprocating_csv_from_logs(
    state: RunState,
    relacao: float,
    raio_mm: float,
    distance_interval_mm: float,
) -> None:
    """Gera _T, _DP e _M reciprocantes apenas com DLG + encoder externo."""
    del relacao  # A relacao mecanica nao participa mais da medicao.
    del raio_mm
    if not os.path.isfile(state.dlg_csv) or not os.path.isfile(state.encoder_state_csv):
        raise RuntimeError("CSV completo do DLG ou estado do encoder externo ausente.")
    interval_mm = float(distance_interval_mm)
    if interval_mm <= 0.0:
        raise RuntimeError("Intervalo de distancia invalido para o reciprocante.")

    def as_float(value, default=None):
        try:
            text_value = str(value).strip()
            if not text_value or text_value.upper() == "NULL":
                return default
            number = float(text_value)
            return number if math.isfinite(number) else default
        except Exception:
            return default

    def as_int(value, default=None):
        try:
            return int(str(value).strip())
        except Exception:
            return default

    def fmt(value, digits=6):
        return "NULL" if value is None else f"{float(value):.{digits}f}"

    event_path = os.path.join(os.path.dirname(state.drive_csv), "a5_speed_events.log")
    physical_events = []
    segment_by_stroke = {}
    done_event = None
    if os.path.isfile(event_path):
        with open(event_path, "r", encoding="ascii", errors="replace") as stream:
            for line in stream:
                fields = _event_fields(line)
                if "RECIP_SHADOW_PHYSICAL_REVERSAL " in line:
                    qpc = as_int(fields.get("qpc"))
                    stroke = as_int(fields.get("stroke"))
                    if qpc is not None and stroke is not None:
                        physical_events.append({
                            "qpc": qpc,
                            "stroke": stroke,
                            "extreme_mm": as_float(fields.get("extreme_mm")),
                            "endpoint_error_mm": as_float(fields.get("endpoint_error_mm")),
                        })
                elif "RECIP_ENCODER_TRIGGER " in line:
                    stroke = as_int(fields.get("stroke"))
                    segment = as_int(fields.get("completed_segment"))
                    if stroke is not None and segment is not None:
                        segment_by_stroke[stroke] = segment
                elif "RECIP_ENCODER_DONE " in line:
                    done_event = {
                        "qpc": as_int(fields.get("qpc")),
                        "stroke": as_int(fields.get("strokes")),
                        "segment": as_int(fields.get("completed_segment")),
                    }
                    if done_event["stroke"] is not None and done_event["segment"] is not None:
                        segment_by_stroke[done_event["stroke"]] = done_event["segment"]

    samples = []
    offset_n = float(getattr(state, "dynamic_offset_n", 0.0) or 0.0)
    last_angle = None
    quarantine_count = 0
    with open(state.dlg_csv, "r", newline="", encoding="utf-8") as fdlg, \
         open(state.encoder_state_csv, "r", newline="", encoding="utf-8") as fenc:
        rd_dlg = csv.DictReader(fdlg)
        rd_enc = csv.DictReader(fenc)
        for dlg_row, enc_row in zip_longest(rd_dlg, rd_enc):
            if dlg_row is None or enc_row is None:
                raise RuntimeError("DLG e encoder externo possuem quantidades diferentes de linhas.")
            idx = as_int(dlg_row.get("idx"))
            enc_idx = as_int(enc_row.get("idx"))
            if idx is None or enc_idx != idx:
                raise RuntimeError("DLG e encoder externo perderam alinhamento por indice.")
            t_s = as_float(dlg_row.get("t_s"))
            qpc = as_int(dlg_row.get("t_qpc"))
            accepted = as_int(enc_row.get("accepted"), 0) == 1
            dlg_ok = as_int(dlg_row.get("err"), 1) == 0
            relative_mm = as_float(enc_row.get("relative_mm"))
            disk_rpm = as_float(enc_row.get("disk_rpm"))
            unwrapped = as_float(enc_row.get("unwrapped_deg"))
            if accepted and unwrapped is not None:
                last_angle = unwrapped % 360.0
            isolated = 0 if accepted else 1
            quarantine_count += isolated
            channels = [as_float(dlg_row.get(f"ch{i}")) for i in range(1, 9)]
            if channels[0] is not None:
                channels[0] -= offset_n
            atrito = as_float(dlg_row.get("atrito"))
            if state.force_normal_n > 0.0 and channels[0] is not None:
                atrito = channels[0] / state.force_normal_n
            samples.append({
                "idx": idx,
                "qpc": qpc,
                "t": t_s,
                "accepted": accepted,
                "dlg_ok": dlg_ok,
                "pos": relative_mm,
                "rpm": disk_rpm,
                "angle": last_angle,
                "isolated": isolated,
                "channels": channels,
                "atr": atrito,
                "valid_force": dlg_ok and accepted and atrito is not None,
            })
    if not samples:
        raise RuntimeError("Nenhuma amostra DLG disponivel para o reciprocante.")

    first_valid = next(
        (i for i, sample in enumerate(samples)
         if sample["accepted"] and sample["pos"] is not None and sample["t"] is not None),
        None,
    )
    if first_valid is None:
        raise RuntimeError("Nenhuma amostra valida do encoder externo no reciprocante.")
    qpcs = [sample["qpc"] if sample["qpc"] is not None else -1 for sample in samples]

    def nearest_sample_index(qpc):
        if qpc is None:
            return None
        pos = bisect.bisect_left(qpcs, qpc)
        choices = [candidate for candidate in (pos - 1, pos) if 0 <= candidate < len(samples)]
        if not choices:
            return None
        return min(choices, key=lambda candidate: abs(qpcs[candidate] - qpc))

    boundaries = []
    for event in sorted(physical_events, key=lambda item: item["stroke"]):
        confirmation_index = nearest_sample_index(event["qpc"])
        sample_index = confirmation_index
        if confirmation_index is not None and event.get("extreme_mm") is not None:
            search_start = max(
                first_valid,
                confirmation_index - int(max(20.0, float(getattr(state, "rate_hz", 200.0)))),
            )
            candidates = [
                i for i in range(search_start, confirmation_index + 1)
                if samples[i]["accepted"] and samples[i]["pos"] is not None
            ]
            if candidates:
                sample_index = min(
                    candidates,
                    key=lambda i: abs(samples[i]["pos"] - event["extreme_mm"]),
                )
        if sample_index is not None and sample_index > first_valid:
            boundaries.append({**event, "sample_index": sample_index})

    final_idx = getattr(state, "reciprocating_final_encoder_idx", None)
    final_sample_index = None
    if final_idx is not None:
        final_sample_index = next(
            (i for i, sample in enumerate(samples) if sample["idx"] >= final_idx),
            None,
        )
    if final_sample_index is None and done_event and done_event.get("qpc") is not None:
        done_sample_index = nearest_sample_index(done_event["qpc"])
        reversal_candidates = [
            i for i in range((done_sample_index or first_valid) + 1, len(samples))
            if as_int(samples[i].get("isolated"), 0) == 0 and
               i > 0 and samples[i]["pos"] is not None and samples[i - 1]["pos"] is not None
        ]
        # A cauda ja foi limitada pelo detector de estabilizacao. O ultimo ponto
        # valido e, portanto, a melhor ancora conservadora se nao houver indice.
        if reversal_candidates:
            final_sample_index = reversal_candidates[-1]
    if final_sample_index is None:
        final_sample_index = len(samples) - 1

    expected_strokes = int(math.ceil(
        float(state.reciprocating_total_mm) /
        max(float(state.reciprocating_course_mm), 1.0e-12)
    ))
    boundaries = [item for item in boundaries if item["sample_index"] < final_sample_index]
    boundaries = boundaries[:max(0, expected_strokes - 1)]
    boundaries.append({
        "sample_index": final_sample_index,
        "stroke": expected_strokes,
        "endpoint_error_mm": None,
    })

    final_output_index = boundaries[-1]["sample_index"]
    output_samples = samples[:final_output_index + 1]
    with open(state.merge_csv, "w", newline="", encoding="utf-8") as ft:
        writer = csv.writer(ft, delimiter=";", lineterminator="\n")
        writer.writerow([
            "idx", "t_s", "ch1", "ch2", "ch3", "ch4", "ch5", "ch6", "ch7", "ch8",
            "atrito", "pos", "rpm", "dlg_err", "encoder_pos_err", "encoder_rpm_err",
            "PosEncExt", "PosEncExt_Quarentena",
        ])
        for sample in output_samples:
            writer.writerow([
                sample["idx"], fmt(sample["t"]),
                *[fmt(value, 10) for value in sample["channels"]],
                fmt(sample["atr"], 10), fmt(sample["pos"], 10), fmt(sample["rpm"], 10),
                0 if sample["dlg_ok"] else 1,
                0 if sample["accepted"] else 1,
                0 if sample["accepted"] and sample["rpm"] is not None else 1,
                fmt(sample["angle"], 10), sample["isolated"],
            ])

    try:
        edge_pct = max(0.0, min(49.0, float(state.reciprocating_edge_filter_pct)))
    except Exception:
        edge_pct = 0.0
    target_schedule = list(getattr(state, "target_speed_schedule", None) or [])
    stroke_ranges = []
    start = first_valid
    for boundary in boundaries:
        end = boundary["sample_index"]
        if end > start:
            stroke_ranges.append((start, end, boundary))
        start = end

    with open(state.turn_vp_csv, "w", newline="", encoding="utf-8") as fm:
        wm = csv.writer(fm, delimiter=";", lineterminator="\n")
        wm.writerow([
            "TEMPO_(min)", "STROKE", "ATRITO_EFETIVO", "ATRITO_MEDIO",
            "ATRITO_MAX", "POS_MAX", "ATRITO_MIN", "POS_MIN", "LINHAS",
            "VELOCIDADE_MEDIA", "VELOCIDADE_ALVO", "ERRO_CURSO_MM",
        ])
        processed_courses = []
        processed_speeds = []
        for number, (start, end, boundary) in enumerate(stroke_ranges, 1):
            start_sample = samples[start]
            end_sample = samples[end]
            start_pos = start_sample["pos"]
            end_pos = end_sample["pos"]
            actual_course = (
                abs(end_pos - start_pos)
                if start_pos is not None and end_pos is not None else None
            )
            direction = 1 if end_pos is not None and start_pos is not None and end_pos >= start_pos else -1
            values = []
            if actual_course is not None:
                edge_mm = actual_course * edge_pct / 100.0
                for sample in samples[start:end + 1]:
                    if sample["pos"] is None or not sample["valid_force"]:
                        continue
                    local = direction * (sample["pos"] - start_pos)
                    if edge_mm <= local <= actual_course - edge_mm:
                        values.append((local, sample["atr"]))
            rms = mean_abs = force_max = force_min = pos_max = pos_min = None
            if values:
                forces = [item[1] for item in values]
                rms = math.sqrt(sum(value * value for value in forces) / len(forces))
                mean_abs = sum(abs(value) for value in forces) / len(forces)
                max_item = max(values, key=lambda item: item[1])
                min_item = min(values, key=lambda item: item[1])
                pos_max, force_max = max_item
                pos_min, force_min = min_item
            duration = (
                end_sample["t"] - start_sample["t"]
                if end_sample["t"] is not None and start_sample["t"] is not None else None
            )
            speed = actual_course / duration if actual_course is not None and duration and duration > 0 else None
            if actual_course is not None:
                processed_courses.append(actual_course)
            if speed is not None:
                processed_speeds.append(speed)
            segment = segment_by_stroke.get(boundary.get("stroke", number), 0)
            target_speed = None
            if 0 <= segment < len(target_schedule):
                target_speed = as_float(target_schedule[segment][0])
            wm.writerow([
                fmt(start_sample["t"] / 60.0 if start_sample["t"] is not None else None),
                number, fmt(rms), fmt(mean_abs), fmt(force_max), fmt(pos_max),
                fmt(force_min), fmt(pos_min), len(values), fmt(speed), fmt(target_speed),
                fmt(actual_course - float(state.reciprocating_course_mm)
                    if actual_course is not None else None),
            ])
    nominal_course = float(state.reciprocating_course_mm)
    course_errors = [value - nominal_course for value in processed_courses]
    state.reciprocating_processing_summary = {
        "strokes": len(processed_courses),
        "course_mean_mm": (
            sum(processed_courses) / len(processed_courses)
            if processed_courses else None
        ),
        "course_min_mm": min(processed_courses) if processed_courses else None,
        "course_max_mm": max(processed_courses) if processed_courses else None,
        "course_error_mean_mm": (
            sum(course_errors) / len(course_errors) if course_errors else None
        ),
        "course_abs_error_mean_mm": (
            sum(abs(value) for value in course_errors) / len(course_errors)
            if course_errors else None
        ),
        "course_abs_error_max_mm": (
            max(abs(value) for value in course_errors) if course_errors else None
        ),
        "speed_mean_mm_s": (
            sum(processed_speeds) / len(processed_speeds)
            if processed_speeds else None
        ),
        "speed_min_mm_s": min(processed_speeds) if processed_speeds else None,
        "speed_max_mm_s": max(processed_speeds) if processed_speeds else None,
    }

    def new_acc(start_t=None):
        return {"t": start_t, "n": 0, "fail": 0, "values": [], "rpm": []}

    def add_acc(acc, sample):
        if acc["t"] is None:
            acc["t"] = sample["t"]
        acc["n"] += 1
        if sample["valid_force"]:
            acc["values"].append(sample["atr"])
            if sample["rpm"] is not None:
                acc["rpm"].append(abs(sample["rpm"]))
        else:
            acc["fail"] += 1

    with open(state.turn_dist_csv, "w", newline="", encoding="utf-8") as fdp:
        wd = csv.writer(fdp, delimiter=";", lineterminator="\n")
        wd.writerow([
            "distancia_mm", "t_s_inicio", "atrito_med", "atrito_min", "atrito_max",
            "rpm_medio_intervalo", "velocidade_media_mm_s", "n_total_pontos",
            "n_falhas", "n_validas", "pct_perda",
        ])
        next_boundary = interval_mm
        base_distance = 0.0
        previous_t = samples[first_valid]["t"]
        previous_cross_t = previous_t
        acc = new_acc(previous_t)
        for start, end, _boundary in stroke_ranges:
            origin = samples[start]["pos"]
            endpoint = samples[end]["pos"]
            if origin is None or endpoint is None:
                continue
            direction = 1 if endpoint >= origin else -1
            course = abs(endpoint - origin)
            previous_progress = base_distance
            previous_sample_t = samples[start]["t"]
            for sample in samples[start:end + 1]:
                add_acc(acc, sample)
                if sample["pos"] is None or sample["t"] is None:
                    continue
                local = max(0.0, min(course, direction * (sample["pos"] - origin)))
                progress = base_distance + local
                while progress >= next_boundary and previous_progress < next_boundary:
                    fraction = (
                        (next_boundary - previous_progress) / (progress - previous_progress)
                        if progress > previous_progress else 1.0
                    )
                    crossing_t = (
                        previous_sample_t + fraction * (sample["t"] - previous_sample_t)
                        if previous_sample_t is not None else sample["t"]
                    )
                    values = acc["values"]
                    rpm_values = acc["rpm"]
                    duration = crossing_t - previous_cross_t if previous_cross_t is not None else None
                    wd.writerow([
                        fmt(next_boundary), fmt(acc["t"]),
                        fmt(sum(abs(value) for value in values) / len(values)
                            if values else None),
                        fmt(min(values) if values else None), fmt(max(values) if values else None),
                        fmt(sum(rpm_values) / len(rpm_values) if rpm_values else None),
                        fmt(interval_mm / duration if duration and duration > 0 else None),
                        acc["n"], acc["fail"], len(values),
                        fmt(100.0 * acc["fail"] / acc["n"] if acc["n"] else 0.0, 3),
                    ])
                    previous_cross_t = crossing_t
                    next_boundary += interval_mm
                    acc = new_acc(crossing_t)
                previous_progress = max(previous_progress, progress)
                previous_sample_t = sample["t"]
            base_distance += course

    state.encoder_quarantine_samples = quarantine_count
    state.encoder_quarantine_fraction = quarantine_count / len(samples)


def _build_continuous_encoder_outputs(state: RunState) -> None:
    """Gera _T, _DP e _VP continuos exclusivamente do DLG + CH3 externo."""
    if not os.path.isfile(state.dlg_csv) or not os.path.isfile(state.encoder_state_csv):
        raise RuntimeError("CSV completo do DLG ou estado do encoder externo ausente.")
    radius_mm = float(state.raio_mm)
    interval_mm = float(state.distance_interval_mm)
    if radius_mm <= 0.0 or interval_mm <= 0.0:
        raise RuntimeError("Raio/intervalo invalidos para processamento continuo.")
    circumference_mm = 2.0 * math.pi * radius_mm

    def as_float(value, default=None):
        try:
            text_value = str(value).strip()
            if not text_value or text_value.upper() == "NULL":
                return default
            number = float(text_value)
            return number if math.isfinite(number) else default
        except Exception:
            return default

    def as_int(value, default=None):
        try:
            return int(str(value).strip())
        except Exception:
            return default

    def new_acc(start_t=None):
        return {
            "t_start": start_t,
            "n_total": 0,
            "n_fail": 0,
            "n_valid": 0,
            "sum": 0.0,
            "min": None,
            "max": None,
        }

    def add_acc(acc, t_s, value, valid):
        if acc["t_start"] is None:
            acc["t_start"] = t_s
        acc["n_total"] += 1
        if not valid or value is None:
            acc["n_fail"] += 1
            return
        acc["n_valid"] += 1
        acc["sum"] += value
        acc["min"] = value if acc["min"] is None else min(acc["min"], value)
        acc["max"] = value if acc["max"] is None else max(acc["max"], value)

    def emit_acc(writer, boundary, acc, crossing_t, previous_crossing_t):
        duration = crossing_t - previous_crossing_t
        segment_distance = boundary[1]
        speed = segment_distance / duration if duration > 0.0 else None
        rpm = speed * 60.0 / circumference_mm if speed is not None else None
        loss_pct = 100.0 * acc["n_fail"] / acc["n_total"] if acc["n_total"] else 0.0
        mean = acc["sum"] / acc["n_valid"] if acc["n_valid"] else None
        writer.writerow([
            f"{boundary[0]:.6f}",
            f"{acc['t_start']:.6f}" if acc["t_start"] is not None else "NULL",
            f"{mean:.6f}" if mean is not None else "NULL",
            f"{acc['min']:.6f}" if acc["min"] is not None else "NULL",
            f"{acc['max']:.6f}" if acc["max"] is not None else "NULL",
            f"{rpm:.6f}" if rpm is not None else "NULL",
            f"{speed:.6f}" if speed is not None else "NULL",
            acc["n_total"], acc["n_fail"], acc["n_valid"], f"{loss_pct:.3f}",
        ])

    with open(state.dlg_csv, "r", newline="", encoding="utf-8") as fdlg, \
         open(state.encoder_state_csv, "r", newline="", encoding="utf-8") as fenc, \
         open(state.merge_csv, "w", newline="", encoding="utf-8") as ft, \
         open(state.turn_dist_csv, "w", newline="", encoding="utf-8") as fdp, \
         open(state.turn_vp_csv, "w", newline="", encoding="utf-8") as fvp:
        rd_dlg = csv.DictReader(fdlg)
        rd_enc = csv.DictReader(fenc)
        wt = csv.writer(ft, delimiter=";", lineterminator="\n")
        wdp = csv.writer(fdp, delimiter=";", lineterminator="\n")
        wvp = csv.writer(fvp, delimiter=";", lineterminator="\n")
        wt.writerow([
            "idx", "t_s", "ch1", "ch2", "ch3", "ch4", "ch5", "ch6", "ch7", "ch8",
            "atrito", "pos", "rpm", "dlg_err", "encoder_pos_err", "encoder_rpm_err",
            "PosEncExt", "PosEncExt_Quarentena",
        ])
        process_header = [
            "distancia_mm", "t_s_inicio", "atrito_med", "atrito_min", "atrito_max",
            "rpm_medio_intervalo", "velocidade_media_mm_s", "n_total_pontos",
            "n_falhas", "n_validas", "pct_perda",
        ]
        wdp.writerow(process_header)
        wvp.writerow([
            "volta_n", "t_s_inicio", "atrito_med", "atrito_min", "atrito_max",
            "rpm_medio_volta", "velocidade_media_mm_s", "n_total_pontos",
            "n_falhas", "n_validas", "pct_perda",
        ])

        next_distance = interval_mm
        next_turn = 1.0
        distance_acc = new_acc()
        turn_acc = new_acc()
        previous_progress = None
        previous_t_s = None
        distance_cross_t = None
        turn_cross_t = None
        last_accepted_angle = None
        quarantine_count = 0
        total_count = 0
        direction_tracker = _ContinuousDirectionTracker()
        target_cross_t = None

        for dlg_row, enc_row in zip_longest(rd_dlg, rd_enc):
            if dlg_row is None or enc_row is None:
                raise RuntimeError("DLG e estado do encoder possuem quantidades diferentes de linhas.")
            dlg_idx = as_int(dlg_row.get("idx"))
            enc_idx = as_int(enc_row.get("idx"))
            if dlg_idx is None or enc_idx is None or dlg_idx != enc_idx:
                raise RuntimeError("DLG e estado do encoder perderam alinhamento por indice.")
            t_s = as_float(dlg_row.get("t_s"))
            if t_s is None:
                continue
            accepted = as_int(enc_row.get("accepted"), 0) == 1
            dlg_ok = as_int(dlg_row.get("err"), 1) == 0
            relative_mm = as_float(enc_row.get("relative_mm"))
            progress = direction_tracker.update(
                t_s, relative_mm, accepted and relative_mm is not None
            )
            disk_rpm = as_float(enc_row.get("disk_rpm"))
            unwrapped = as_float(enc_row.get("unwrapped_deg"))
            if accepted and unwrapped is not None:
                last_accepted_angle = unwrapped % 360.0
            isolated = 0 if accepted else 1
            quarantine_count += isolated
            total_count += 1
            ch1 = as_float(dlg_row.get("ch1"))
            atrito = as_float(dlg_row.get("atrito"))
            if state.force_normal_n > 0.0 and ch1 is not None:
                atrito = ch1 / state.force_normal_n
            sample_valid = dlg_ok and accepted and atrito is not None
            wt.writerow([
                dlg_idx, f"{t_s:.6f}",
                *[dlg_row.get(f"ch{i}", "NULL") or "NULL" for i in range(1, 9)],
                f"{atrito:.10g}" if atrito is not None else "NULL",
                f"{relative_mm:.10g}" if relative_mm is not None else "NULL",
                f"{abs(disk_rpm):.10g}" if disk_rpm is not None else "NULL",
                0 if dlg_ok else 1,
                0 if accepted else 1,
                0 if accepted and disk_rpm is not None else 1,
                f"{last_accepted_angle:.10g}" if last_accepted_angle is not None else "NULL",
                isolated,
            ])
            add_acc(distance_acc, t_s, atrito, sample_valid)
            add_acc(turn_acc, t_s, atrito, sample_valid)

            if accepted and progress is not None:
                if previous_progress is None or previous_t_s is None:
                    previous_progress = progress
                    previous_t_s = t_s
                    distance_cross_t = t_s
                    turn_cross_t = t_s
                    continue
                if progress > previous_progress and t_s > previous_t_s:
                    while progress >= next_distance and previous_progress < next_distance:
                        fraction = (next_distance - previous_progress) / (progress - previous_progress)
                        crossing_t = previous_t_s + fraction * (t_s - previous_t_s)
                        emit_acc(
                            wdp, (next_distance, interval_mm), distance_acc,
                            crossing_t, distance_cross_t,
                        )
                        distance_cross_t = crossing_t
                        next_distance += interval_mm
                        distance_acc = new_acc(crossing_t)
                    next_turn_distance = next_turn * circumference_mm
                    while progress >= next_turn_distance and previous_progress < next_turn_distance:
                        fraction = (next_turn_distance - previous_progress) / (progress - previous_progress)
                        crossing_t = previous_t_s + fraction * (t_s - previous_t_s)
                        emit_acc(
                            wvp, (next_turn, circumference_mm), turn_acc,
                            crossing_t, turn_cross_t,
                        )
                        turn_cross_t = crossing_t
                        next_turn += 1.0
                        next_turn_distance = next_turn * circumference_mm
                        turn_acc = new_acc(crossing_t)
                target_mm = float(getattr(state, "continuous_target_mm", 0.0) or 0.0)
                target_reached = target_mm > 0.0 and progress >= target_mm
                if target_reached:
                    if progress > previous_progress and t_s > previous_t_s:
                        fraction = (target_mm - previous_progress) / (progress - previous_progress)
                        target_cross_t = previous_t_s + fraction * (t_s - previous_t_s)
                    else:
                        target_cross_t = t_s
                previous_progress = progress
                previous_t_s = t_s
                if target_reached:
                    break

    state.encoder_quarantine_samples = quarantine_count
    state.encoder_quarantine_fraction = quarantine_count / total_count if total_count else 0.0
    state.continuous_rebuild_direction = direction_tracker.direction
    state.continuous_rebuild_direction_lock_t_s = direction_tracker.lock_t_s
    state.continuous_rebuild_target_t_s = target_cross_t
    state.continuous_rebuild_progress_mm = direction_tracker.max_progress_mm
    state.continuous_rebuild_fault = direction_tracker.fault


def _rebuild_turn_csv_from_logs(state: RunState) -> None:
    """
    Regera arquivos _DP e _VP/_M em modo offline (sem IPC) usando logs finais.

    Esse passo roda ao final do ensaio para garantir consistencia dos dados
    por distancia, volta ou stroke, mesmo se o processamento em tempo real atrasar.
    """
    if not state:
        return
    processing_dlg_csv = _processing_dlg_csv(state)
    if not os.path.exists(processing_dlg_csv) or not os.path.exists(state.drive_csv):
        return
    # Reprocessa no final em Python para manter consistencia com o grafico 3
    # em tempo real e evitar divergencia de parametros legados do agregador C.
    try:
        relacao = float(state.relacao) if state.relacao > 0 else 1.0
    except Exception:
        relacao = 1.0
    try:
        raio_mm = float(state.raio_mm) if state.raio_mm > 0 else 0.0
    except Exception:
        raio_mm = 0.0
    try:
        distance_interval_mm = float(state.distance_interval_mm) if state.distance_interval_mm > 0 else 10.0
    except Exception:
        distance_interval_mm = 10.0
    if bool(getattr(state, "reciprocating", False)):
        _rebuild_reciprocating_csv_from_logs(state, relacao, raio_mm, distance_interval_mm)
        return
    cycles_per_motor_rev = 1.0
    rpm_dir_deadband = 5.0

    def _to_int(txt, default=None):
        try:
            if txt is None:
                return default
            t = str(txt).strip()
            if not t or t.upper() == "NULL":
                return default
            return int(t)
        except Exception:
            return default

    def _to_float(txt, default=None):
        try:
            if txt is None:
                return default
            t = str(txt).strip()
            if not t or t.upper() == "NULL":
                return default
            return float(t)
        except Exception:
            return default

    turn_dist_csv = state.turn_dist_csv
    turn_vp_csv = state.turn_vp_csv

    def _acc_reset():
        return {
            "n_total": 0,
            "n_fail": 0,
            "n_valid": 0,
            "t_start_s": None,
            "atr_sum": 0.0,
            "atr_min": None,
            "atr_max": None,
            "rpm_sum": 0.0,
            "rpm_cnt": 0,
        }

    def _emit_row(writer, x_value, acc):
        pct_loss = (100.0 * acc["n_fail"] / acc["n_total"]) if acc["n_total"] > 0 else 0.0
        atr_med = (acc["atr_sum"] / acc["n_valid"]) if acc["n_valid"] > 0 else None
        rpm_med = (acc["rpm_sum"] / acc["rpm_cnt"]) if acc["rpm_cnt"] > 0 else None
        vel_med_mm_s = pin_speed_mm_s_from_rpm(rpm_med, raio_mm, relacao) if rpm_med is not None else None
        t_start_s = acc.get("t_start_s")
        writer.writerow([
            x_value,
            f"{t_start_s:.6f}" if t_start_s is not None else "NULL",
            f"{atr_med:.6f}" if atr_med is not None else "NULL",
            f"{acc['atr_min']:.6f}" if acc["atr_min"] is not None else "NULL",
            f"{acc['atr_max']:.6f}" if acc["atr_max"] is not None else "NULL",
            f"{rpm_med:.6f}" if rpm_med is not None else "NULL",
            f"{vel_med_mm_s:.6f}" if vel_med_mm_s is not None else "NULL",
            acc["n_total"], acc["n_fail"], acc["n_valid"], f"{pct_loss:.3f}"
        ])

    with open(turn_dist_csv, "w", newline="", encoding="utf-8") as fdist, \
         open(turn_vp_csv, "w", newline="", encoding="utf-8") as fturn:
        wd = csv.writer(fdist, delimiter=";", lineterminator="\n")
        wt = csv.writer(fturn, delimiter=";", lineterminator="\n")

        wd.writerow([
            "distancia_mm", "t_s_inicio", "atrito_med", "atrito_min", "atrito_max", "rpm_medio_intervalo", "velocidade_media_mm_s",
            "n_total_pontos", "n_falhas", "n_validas", "pct_perda"
        ])
        wt.writerow([
            "volta_n", "t_s_inicio", "atrito_med", "atrito_min", "atrito_max", "rpm_medio_volta", "velocidade_media_mm_s",
            "n_total_pontos", "n_falhas", "n_validas", "pct_perda"
        ])

        with open(processing_dlg_csv, "r", newline="", encoding="utf-8") as fdlg, \
             open(state.drive_csv, "r", newline="", encoding="utf-8") as fdrv:
            rd_d = csv.reader(fdlg)
            rd_r = csv.reader(fdrv)
            next(rd_d, None)
            next(rd_r, None)

            drow = None
            rrow = None
            have_d = False
            have_r = False

            prev_pos = 0.0
            prev_pos_valid = False
            prev_t_s = 0.0
            prev_t_valid = False
            dir_sign = 1
            cum_dist_mm = 0.0
            cum_turn = 0.0
            next_boundary_mm = distance_interval_mm
            next_boundary_turn = 1.0

            acc_dist = _acc_reset()
            acc_turn = _acc_reset()

            while True:
                if not have_d:
                    try:
                        drow = next(rd_d)
                        have_d = True
                    except StopIteration:
                        break
                if not have_r:
                    try:
                        rrow = next(rd_r)
                        have_r = True
                    except StopIteration:
                        break

                idx_d = _to_int(drow[0] if drow and len(drow) > 0 else None, None)
                idx_r = _to_int(rrow[0] if rrow and len(rrow) > 0 else None, None)
                if idx_d is None:
                    have_d = False
                    continue
                if idx_r is None:
                    have_r = False
                    continue

                if idx_d < idx_r:
                    have_d = False
                    continue
                if idx_r < idx_d:
                    have_r = False
                    continue

                # idx alinhado
                have_d = False
                have_r = False

                dlg_err = _to_int(drow[-1] if drow else None, 1)
                ch1 = _to_float(drow[3] if drow and len(drow) > 3 else None, None)
                atr = _to_float(drow[11] if drow and len(drow) > 11 else None, None)
                if ch1 is not None:
                    ch1 -= float(getattr(state, "dynamic_offset_n", 0.0) or 0.0)
                if state.force_normal_n and state.force_normal_n > 0 and ch1 is not None:
                    atr = ch1 / state.force_normal_n
                atr_ok = (dlg_err == 0 and atr is not None)

                t_s_dlg = _to_float(drow[2] if drow and len(drow) > 2 else None, None)
                t_s_drv = _to_float(rrow[2] if rrow and len(rrow) > 2 else None, None)
                t_s_group = t_s_drv if t_s_drv is not None else t_s_dlg
                pos_err = _to_int(rrow[5] if rrow and len(rrow) > 5 else None, 1)
                rpm_err = _to_int(rrow[6] if rrow and len(rrow) > 6 else None, 1)
                pos = _to_float(rrow[3] if rrow and len(rrow) > 3 else None, None)
                rpm = _to_float(rrow[4] if rrow and len(rrow) > 4 else None, None)
                pos_mod = _to_float(rrow[7] if rrow and len(rrow) > 7 else None, 65536.0)
                if pos_mod is None or pos_mod <= 1.0:
                    pos_mod = 65536.0

                pos_ok = (pos_err == 0 and pos is not None)
                rpm_ok = (rpm_err == 0 and rpm is not None)

                acc_dist["n_total"] += 1
                acc_turn["n_total"] += 1
                if acc_dist["t_start_s"] is None:
                    acc_dist["t_start_s"] = t_s_group
                if acc_turn["t_start_s"] is None:
                    acc_turn["t_start_s"] = t_s_group
                if atr_ok and pos_ok:
                    acc_dist["n_valid"] += 1
                    acc_turn["n_valid"] += 1
                    acc_dist["atr_sum"] += atr
                    acc_turn["atr_sum"] += atr
                    acc_dist["atr_min"] = atr if acc_dist["atr_min"] is None else min(acc_dist["atr_min"], atr)
                    acc_turn["atr_min"] = atr if acc_turn["atr_min"] is None else min(acc_turn["atr_min"], atr)
                    acc_dist["atr_max"] = atr if acc_dist["atr_max"] is None else max(acc_dist["atr_max"], atr)
                    acc_turn["atr_max"] = atr if acc_turn["atr_max"] is None else max(acc_turn["atr_max"], atr)
                    if rpm_ok:
                        acc_dist["rpm_sum"] += rpm
                        acc_turn["rpm_sum"] += rpm
                        acc_dist["rpm_cnt"] += 1
                        acc_turn["rpm_cnt"] += 1
                else:
                    acc_dist["n_fail"] += 1
                    acc_turn["n_fail"] += 1

                if pos_ok and prev_pos_valid:
                    raw_diff = pos - prev_pos
                    d_eff = 0.0
                    if rpm_ok and abs(rpm) >= rpm_dir_deadband:
                        dir_sign = 1 if rpm >= 0.0 else -1

                    if dir_sign >= 0:
                        d_eff = raw_diff
                        if d_eff < (-0.5 * pos_mod):
                            d_eff += pos_mod
                        elif d_eff < 0.0:
                            d_eff = 0.0
                    else:
                        d_eff = raw_diff
                        if d_eff > (0.5 * pos_mod):
                            d_eff -= pos_mod
                        elif d_eff > 0.0:
                            d_eff = 0.0

                    if d_eff != 0.0:
                        dt_s = 0.0
                        if prev_t_valid and t_s_drv is not None:
                            dt_s = t_s_drv - prev_t_s
                            if dt_s <= 0.0 or dt_s > 1.0:
                                dt_s = 0.0
                        if rpm_ok and dt_s > 0.0:
                            max_step = pos_mod * (abs(rpm) / 60.0) * dt_s * 3.0 + 5.0
                        elif rpm_ok:
                            max_step = pos_mod * (abs(rpm) / 60.0) * 0.05 * 3.0 + 5.0
                        else:
                            max_step = pos_mod * 0.05
                        if abs(d_eff) > max_step:
                            d_eff = 0.0

                    motor_turn_inc = abs(d_eff) / (pos_mod * cycles_per_motor_rev)
                    pin_turn_inc = motor_turn_inc / relacao
                    dist_inc_mm = pin_turn_inc * (2.0 * 3.141592653589793 * raio_mm)
                    if pin_turn_inc > 0.0:
                        cum_turn += pin_turn_inc
                    if dist_inc_mm > 0.0:
                        cum_dist_mm += dist_inc_mm

                    while cum_dist_mm >= next_boundary_mm:
                        if acc_dist["t_start_s"] is None:
                            acc_dist["t_start_s"] = t_s_group
                        _emit_row(wd, f"{next_boundary_mm:.6f}", acc_dist)
                        next_boundary_mm += distance_interval_mm
                        acc_dist = _acc_reset()

                    while cum_turn >= next_boundary_turn:
                        if acc_turn["t_start_s"] is None:
                            acc_turn["t_start_s"] = t_s_group
                        _emit_row(wt, f"{next_boundary_turn:.6f}", acc_turn)
                        next_boundary_turn += 1.0
                        acc_turn = _acc_reset()

                if pos_ok:
                    prev_pos = pos
                    prev_pos_valid = True
                    prev_t_s = t_s_drv if t_s_drv is not None else prev_t_s
                    prev_t_valid = (t_s_drv is not None)


def _merge_csv_fallback(dlg_csv: str, drive_csv: str, out_csv: str) -> None:
    """
    Merge de alternativa implementado em Python.

    Objetivo:
    - Manter o pipeline operacional mesmo sem merge_logs.exe.
    - Preservar a estrutura de colunas esperada no resultado final.

    Estrutura de saida:
      idx,t_s,ch1..ch8,atrito,pos,rpm,dlg_err,drive_pos_err,drive_rpm_err

    Regras:
    - Faz merge por indice de linha (zip_longest).
    - Onde faltar dado, escreve "NULL" nos campos de medicao.
    - Campos de erro ausentes recebem "1" como alternativa conservadora.
    """
    with open(dlg_csv, "r", newline="", encoding="utf-8") as fdlg, \
         open(drive_csv, "r", newline="", encoding="utf-8") as fdrv, \
         open(out_csv, "w", newline="", encoding="utf-8") as fout:
        rd_dlg = csv.reader(fdlg)
        rd_drv = csv.reader(fdrv)
        w = csv.writer(fout, delimiter=";", lineterminator="\n")

        # Ignora cabecalhos dos dois arquivos de entrada.
        next(rd_dlg, None)
        next(rd_drv, None)

        w.writerow(["idx", "t_s", "ch1", "ch2", "ch3", "ch4", "ch5", "ch6", "ch7", "ch8", "atrito", "pos", "rpm", "dlg_err", "drive_pos_err", "drive_rpm_err"])

        idx_fallback = 0
        for drow, rrow in zip_longest(rd_dlg, rd_drv, fillvalue=None):
            dlg = drow if drow else []
            drv = rrow if rrow else []

            idx = dlg[0] if len(dlg) > 0 and dlg[0] else (drv[0] if len(drv) > 0 and drv[0] else str(idx_fallback))
            t_s = dlg[2] if len(dlg) > 2 and dlg[2] else (drv[2] if len(drv) > 2 and drv[2] else "NULL")

            ch = []
            for i in range(8):
                # Em dlg.csv, ch1 inicia na coluna 3 (indice base 0).
                col = 3 + i
                ch.append(dlg[col] if len(dlg) > col and dlg[col] else "NULL")

            atrito = "NULL"
            if len(dlg) > 12:
                atrito = dlg[11] if dlg[11] else "NULL"
                dlg_err = dlg[12] if dlg[12] else "1"
            else:
                dlg_err = dlg[11] if len(dlg) > 11 and dlg[11] else "1"

            pos = drv[3] if len(drv) > 3 and drv[3] else "NULL"
            rpm = drv[4] if len(drv) > 4 and drv[4] else "NULL"
            drv_pos_err = drv[5] if len(drv) > 5 and drv[5] else "1"
            drv_rpm_err = drv[6] if len(drv) > 6 and drv[6] else "1"

            w.writerow([idx, t_s, *ch, atrito, pos, rpm, dlg_err, drv_pos_err, drv_rpm_err])
            idx_fallback += 1


def stop_run(state: Optional[RunState], manual: bool = False) -> None:
    """
    Para os dois processos com prioridade para encerramento gracioso.

    Estrategia:
    1) Envia STOP via IPC para DLG e Drive (melhor esforco).
    2) Aguarda curto periodo para flush/fechamento limpo.
    3) Se ainda vivos, aplica terminate() e depois kill() como ultimo recurso.

    Parametros:
        state: estado do ensaio em execucao.
        manual: marca interrupcao solicitada pelo operador; nesse caso o
            continuo preserva os dados parciais sem classificar como deadline.
    """
    if not state:
        return
    if manual:
        state.manual_stop_requested = True
    procs = (state.dlg_proc, state.drive_proc)

    # 1) Encerramento gracioso via IPC.
    for p in procs:
        _send_ipc(p, "STOP")

    # 2) Aguarda ambos fecharem para reduzir risco de arquivo parcial.
    deadline = time.time() + 6.0
    while time.time() < deadline:
        if all((not p) or (p.poll() is not None) for p in procs):
            return
        time.sleep(0.05)

    # 3) Alternativa forcada.
    for p in procs:
        if p and p.poll() is None:
            try:
                p.terminate()
                p.wait(timeout=1.5)
            except Exception:
                try:
                    p.kill()
                except Exception:
                    pass


def _wait_ready(proc: subprocess.Popen, tag: str, timeout_s: float = 5.0) -> bool:
    """
    Aguarda linha "READY" emitida pelo logger (melhor esforco).

    Importante:
    - Retorna True quando READY foi visto no stdout.
    - Retorna False em timeout/falha/saida precoce.

    Parametros:
        proc: processo alvo.
        tag: identificador textual (mantido para logs futuros).
        timeout_s: tempo maximo de espera.

    Retorna:
        True quando READY foi recebido; False caso contrario.
    """
    if not proc or not proc.stdout:
        return False
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        if proc.poll() is not None:
            return False
        line = proc.stdout.readline()
        if not line:
            time.sleep(0.05)
            continue
        if "READY" in line:
            return True
    return False


def _send_start(proc: subprocess.Popen) -> None:
    _send_ipc(proc, "START")


def _send_ipc(proc: Optional[subprocess.Popen], command: str) -> bool:
    """
    Envia comando IPC de uma linha para o processo.

    Parametros:
        proc: processo alvo (com stdin aberto).
        command: comando textual sem quebra de linha.

    Retorna:
        True quando o envio e flush foram bem-sucedidos; False caso contrario.
    """
    if not proc or proc.poll() is not None or not proc.stdin:
        return False
    try:
        proc.stdin.write(command + "\n")
        proc.stdin.flush()
        return True
    except Exception:
        return False


def pause_run(state: Optional[RunState]) -> None:
    """
    Pausa os dois loggers.

    Ordem adotada:
    - Drive primeiro, para solicitar desaceleracao/parada do motor o quanto antes.
    - DLG depois, para acompanhar a pausa do pipeline.
    """
    if not state:
        return
    _send_ipc(state.drive_proc, "PAUSE")
    _send_ipc(state.dlg_proc, "PAUSE")
    state.paused = True


def resume_run(state: Optional[RunState]) -> None:
    """
    Retoma os dois loggers apos pausa.

    Ordem adotada:
    - DLG primeiro para preservar alinhamento temporal do merge.
    - Drive depois para retomar movimento.
    """
    if not state:
        return
    _send_ipc(state.dlg_proc, "RESUME")
    _send_ipc(state.drive_proc, "RESUME")
    state.paused = False


def _wait_data_ready(proc: subprocess.Popen, timeout_s: float = 5.0) -> bool:
    """
    Aguarda confirmacao de primeira amostra do logger DLG.

    Linhas esperadas no stdout:
    - DATA_OK: primeira amostra valida recebida.
    - DATA_TIMEOUT: nao recebeu amostra no tempo interno do logger.

    Parametros:
        proc: processo do logger DLG.
        timeout_s: tempo maximo de espera.

    Retorna:
        True se recebeu DATA_OK; False em timeout/falha.
    """
    if not proc or not proc.stdout:
        return False
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        line = proc.stdout.readline()
        if not line:
            time.sleep(0.05)
            continue
        if "DATA_OK" in line:
            return True
        if "DATA_TIMEOUT" in line:
            return False
    return False
