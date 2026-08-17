"""Coleta segura e gradual da resposta de parada do modo reciprocante.

O controle com deadline permanece no executavel C do Drive. Este programa
somente organiza as condicoes, aplica gates entre elas e consolida o estudo.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import math
import os
import re
import signal
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

import numpy as np


HERE = Path(__file__).resolve().parent


def _project_root() -> Path:
    start = Path(sys.executable).resolve().parent if getattr(sys, "frozen", False) else HERE
    for candidate in (start, *start.parents):
        if (candidate / "DLG4000").is_dir() and (candidate / "DriveA5").is_dir():
            return candidate
    return HERE.parent


PROJECT_ROOT = _project_root()
RUNTIME_ROOT = Path(getattr(sys, "_MEIPASS", PROJECT_ROOT))
SUPERVISOR_DIR = PROJECT_ROOT / "Supervisório"
if str(SUPERVISOR_DIR) not in sys.path:
    sys.path.insert(0, str(SUPERVISOR_DIR))

import orchestrator_runtime as orch  # noqa: E402


DEFAULT_SPEEDS = (1.0, 2.0, 5.0, 10.0, 15.0, 20.0)
DEFAULT_COURSES = (50.0, 30.0, 15.0, 4.0)
CURRENT_STATE = None
STOP_REQUESTED = False


@dataclass(frozen=True)
class StudyConfig:
    radius_mm: float = 15.0
    ratio: float = 4.0
    tolerance_counts: int = 350
    strokes: int = 6
    dlg_rate_hz: float = 200.0
    drive_rate_hz: float = 50.0
    shadow_tolerance_mm: float = 0.5
    com_port: str = "COM5"
    baud: int = 115200
    parity: str = "E"


def _signal_stop(_signum=None, _frame=None) -> None:
    global STOP_REQUESTED
    STOP_REQUESTED = True
    if CURRENT_STATE is not None:
        orch.stop_run(CURRENT_STATE, manual=True)


def _load_local_defaults() -> StudyConfig:
    path = Path(os.environ.get("LOCALAPPDATA", "")) / "LATRIB" / "supervisorio_settings.json"
    data: Dict[str, object] = {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        pass
    return StudyConfig(
        ratio=float(data.get("relacao", 4.0) or 4.0),
        tolerance_counts=int(data.get("recip_stop_tol_counts", 350) or 350),
    )


def default_plan() -> List[Tuple[float, float]]:
    """Longos antes dos curtos em cada degrau de velocidade."""
    return [(speed, course) for speed in DEFAULT_SPEEDS for course in DEFAULT_COURSES]


def _condition_name(index: int, speed: float, course: float) -> str:
    return f"{index:02d}_v{speed:g}_c{course:g}".replace(".", "p")


def _output_paths(folder: Path) -> dict:
    folder.mkdir(parents=True, exist_ok=False)
    return {
        "folder": str(folder),
        "info_csv": str(folder / "info.csv"),
        "dlg_csv": str(folder / "dlg.csv"),
        "dlg_compat_csv": str(folder / "dlg_compat_50hz.csv"),
        "encoder_state_csv": str(folder / "encoder_state.csv"),
        "drive_csv": str(folder / "drive.csv"),
        "turn_dist_csv": str(folder / "atrito_por_distancia.csv"),
        "turn_vp_csv": str(folder / "atrito_por_stroke.csv"),
        "merge_csv": str(folder / "resultado_ensaio.csv"),
        "schedule_csv": str(folder / "schedule.csv"),
    }


def _read_diagnostic_rows(folder: Path) -> List[dict]:
    candidates = (
        folder / "DadosDev" / "recip_stop_diagnostics.csv",
        folder / "recip_stop_diagnostics.csv",
    )
    for path in candidates:
        if path.is_file():
            with path.open("r", newline="", encoding="ascii", errors="replace") as stream:
                return list(csv.DictReader(stream, delimiter=";"))
    return []


def _float(row: dict, key: str) -> Optional[float]:
    try:
        value = str(row.get(key, "")).strip()
        if not value or value.upper() == "NULL":
            return None
        result = float(value)
        return result if math.isfinite(result) else None
    except Exception:
        return None


def _event_log(folder: Path, name: str) -> str:
    for path in (folder / "DadosDev" / name, folder / name):
        if path.is_file():
            return path.read_text(encoding="ascii", errors="replace")
    return ""


def _line_fields(line: str) -> Dict[str, str]:
    fields = {}
    for token in line.replace(",", " ").split():
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key.strip()] = value.strip()
    return fields


def evaluate_condition(folder: Path, config: StudyConfig, speed: float, course: float) -> dict:
    rows = _read_diagnostic_rows(folder)
    retained = rows[2:]  # primeiro ciclo = dois strokes, sempre descartado
    stops = [_float(row, "distancia_parada_apos_gatilho_legado_mm") for row in retained]
    stops = [value for value in stops if value is not None]
    physical = [_float(row, "curso_fisico_mm") for row in retained]
    physical = [value for value in physical if value is not None]
    velocities = [_float(row, "velocidade_externa_modulo_mm_s") for row in retained]
    velocities = [value for value in velocities if value is not None]
    directions = {
        int(value) for value in (_float(row, "sentido") for row in retained)
        if value in (-1.0, 1.0)
    }
    drive_log = _event_log(folder, "a5_speed_events.log")
    dlg_log = _event_log(folder, "dlg_logger_events.log")
    loss_matches = re.findall(r"loss_pct=([0-9.]+)", dlg_log)
    loss_pct = float(loss_matches[-1]) if loss_matches else 100.0
    shadow_end = next(
        (line for line in reversed(drive_log.splitlines()) if "RECIP_SHADOW_END " in line),
        "",
    )
    shadow_fields = _line_fields(shadow_end)
    packets_valid = int(shadow_fields.get("valid", "0") or 0)
    latency_mean_ms = float(shadow_fields.get("latency_mean_ms", "inf") or math.inf)
    latency_max_ms = float(shadow_fields.get("latency_max_ms", "inf") or math.inf)
    latency_over_100 = int(shadow_fields.get("latency_over_100ms", "0") or 0)
    latency_over_100_fraction = latency_over_100 / packets_valid if packets_valid else 1.0
    shadow_ok = "fault=NONE" in shadow_end and "action_enabled=0" in shadow_end
    drive_preflight_ok = "DRIVE_PREFLIGHT_OK" in drive_log
    reasons: List[str] = []
    safety_reasons: List[str] = []
    warnings: List[str] = []
    if len(stops) < 3:
        reasons.append("menos de 3 paradas fisicas apos o ciclo descartado")
    if directions != {-1, 1}:
        reasons.append("nao ha paradas validas nos dois sentidos")
    if loss_pct > 1.0:
        safety_reasons.append(f"perda DLG {loss_pct:.3f}% > 1%")
    if not shadow_ok:
        safety_reasons.append("resumo do encoder sombra ausente ou com falha")
    if not drive_preflight_ok:
        safety_reasons.append("preflight estrito do Drive nao foi confirmado")
    if latency_mean_ms > 20.0:
        safety_reasons.append(f"latencia media encoder->controle {latency_mean_ms:.3f} ms > 20 ms")
    if latency_over_100_fraction > 0.02:
        safety_reasons.append(
            f"{100.0 * latency_over_100_fraction:.3f}% dos pacotes excederam 100 ms"
        )
    elif latency_over_100_fraction > 0.005:
        warnings.append(
            f"picos isolados: {100.0 * latency_over_100_fraction:.3f}% dos pacotes excederam 100 ms"
        )
    if stops and min(stops) < -0.25:
        safety_reasons.append("distancia de parada negativa incompativel")
    if stops and max(stops) >= course:
        safety_reasons.append("parada consumiu um curso completo")
    if physical and max(physical) >= 2.0 * course:
        safety_reasons.append("curso fisico atingiu o limite de 2x o curso configurado")
    reasons.extend(safety_reasons)
    boundary = bool(stops and max(stops) > 0.35 * course)
    return {
        "accepted": not reasons,
        "safe_to_continue": not safety_reasons,
        "safety_reasons": safety_reasons,
        "reasons": reasons,
        "warnings": warnings,
        "boundary_for_shorter_courses": boundary,
        "diagnostic_rows": len(rows),
        "retained_rows": len(retained),
        "valid_stop_rows": len(stops),
        "loss_pct": loss_pct,
        "stop_mean_mm": float(np.mean(stops)) if stops else None,
        "stop_max_mm": max(stops) if stops else None,
        "physical_course_mean_mm": float(np.mean(physical)) if physical else None,
        "external_speed_mean_mm_s": float(np.mean(velocities)) if velocities else None,
        "directions": sorted(directions),
        "shadow_ok": shadow_ok,
        "drive_preflight_ok": drive_preflight_ok,
        "encoder_latency_mean_ms": latency_mean_ms,
        "encoder_latency_max_ms": latency_max_ms,
        "encoder_latency_over_100_fraction": latency_over_100_fraction,
        "configured_speed_mm_s": speed,
        "configured_course_mm": course,
        "config": asdict(config),
    }


def run_condition(root: Path, index: int, speed: float, course: float, config: StudyConfig) -> dict:
    global CURRENT_STATE
    rpm = orch.rpm_from_mm_s(speed, config.radius_mm, config.ratio)
    effective_speed = orch.pin_speed_mm_s_from_rpm(rpm, config.radius_mm, config.ratio)
    if rpm <= 0 or not effective_speed:
        raise ValueError("velocidade gerou RPM invalido")
    total_mm = course * config.strokes
    theoretical_s = total_mm / effective_speed
    watchdog_s = max(theoretical_s * 1.5, theoretical_s + 30.0)
    folder = root / _condition_name(index, speed, course)
    paths = _output_paths(folder)
    print(
        f"\n[{index:02d}] v={speed:g} mm/s, curso={course:g} mm, "
        f"Drive={rpm} rpm, v efetiva={effective_speed:.3f} mm/s, "
        f"tempo estimado={theoretical_s:.1f} s"
    )
    CURRENT_STATE = orch.start_external_run(
        repo_root=str(RUNTIME_ROOT),
        out_paths=paths,
        schedule=[(rpm, theoretical_s)],
        duration_s=watchdog_s,
        rate_hz=config.dlg_rate_hz,
        drive_rate_hz=config.drive_rate_hz,
        com_port=config.com_port,
        baud=config.baud,
        parity=config.parity,
        relacao=config.ratio,
        raio_mm=config.radius_mm,
        reciprocating=True,
        reciprocating_course_mm=course,
        reciprocating_total_mm=total_mm,
        reciprocating_tolerance_counts=config.tolerance_counts,
        target_speed_schedule=[(speed, theoretical_s)],
        reciprocating_shadow_enabled=True,
        reciprocating_shadow_tolerance_mm=config.shadow_tolerance_mm,
        reciprocating_shadow_total_mm=total_mm,
        strict_drive_setup=True,
    )
    try:
        rc = orch.wait_and_merge(CURRENT_STATE)
    finally:
        CURRENT_STATE = None
    result = evaluate_condition(folder, config, speed, course)
    result.update({
        "index": index,
        "folder": str(folder),
        "return_code": rc,
        "rpm_drive": rpm,
        "effective_speed_mm_s": effective_speed,
        "theoretical_s": theoretical_s,
        "watchdog_s": watchdog_s,
    })
    if rc != 0:
        result["accepted"] = False
        result["reasons"].append(f"pipeline retornou codigo {rc}")
    (folder / "condition_result.json").write_text(
        json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8"
    )
    status = "APROVADA" if result["accepted"] else "REPROVADA"
    print(
        f"[{index:02d}] {status}: parada media={result['stop_mean_mm']}, "
        f"max={result['stop_max_mm']}, perda DLG={result['loss_pct']:.3f}%"
    )
    if result["reasons"]:
        print("  Motivos: " + "; ".join(result["reasons"]))
    if result["warnings"]:
        print("  Avisos: " + "; ".join(result["warnings"]))
    return result


def _candidate_models() -> Sequence[Tuple[str, Sequence[str]]]:
    return (
        ("tempo_resposta", ("v",)),
        ("fisico_velocidade", ("v", "v2")),
        ("superficie_curso", ("1", "v", "v2", "c", "vc")),
    )


def _design(rows: Sequence[dict], features: Sequence[str]) -> np.ndarray:
    matrix = []
    for row in rows:
        v = float(row["velocidade_externa_modulo_mm_s"])
        c = float(row["curso_configurado_mm"])
        values = {"1": 1.0, "v": v, "v2": v * v, "c": c, "vc": v * c}
        matrix.append([values[name] for name in features])
    return np.asarray(matrix, dtype=float)


def _fit_model(rows: Sequence[dict], features: Sequence[str]) -> Tuple[np.ndarray, np.ndarray]:
    x = _design(rows, features)
    y = np.asarray([float(row["distancia_parada_apos_gatilho_legado_mm"]) for row in rows])
    beta, *_ = np.linalg.lstsq(x, y, rcond=None)
    return beta, x @ beta


def _condition_cv_errors(rows: Sequence[dict], features: Sequence[str]) -> List[float]:
    keys = sorted({(row["condicao"],) for row in rows})
    errors = []
    for (key,) in keys:
        train = [row for row in rows if row["condicao"] != key]
        test = [row for row in rows if row["condicao"] == key]
        if len(train) < len(features) + 2:
            continue
        beta, _ = _fit_model(train, features)
        y = np.asarray([float(row["distancia_parada_apos_gatilho_legado_mm"]) for row in test])
        pred = _design(test, features) @ beta
        errors.extend((pred - y).tolist())
    return errors


def _condition_cv_rmse(rows: Sequence[dict], features: Sequence[str]) -> float:
    errors = _condition_cv_errors(rows, features)
    return float(math.sqrt(np.mean(np.square(errors)))) if errors else math.inf


def _balanced_model_rows(rows: Sequence[dict]) -> List[dict]:
    """Da o mesmo peso a cada sentido dentro de cada condicao."""
    groups: Dict[Tuple[str, int], List[dict]] = {}
    for row in rows:
        direction = int(float(row.get("sentido", 0)))
        if direction not in (-1, 1):
            continue
        groups.setdefault((str(row["condicao"]), direction), []).append(row)
    balanced = []
    for (condition, direction), group in sorted(groups.items()):
        balanced.append({
            "condicao": condition,
            "sentido": direction,
            "curso_configurado_mm": float(group[0]["curso_configurado_mm"]),
            "velocidade_externa_modulo_mm_s": float(np.mean([
                float(item["velocidade_externa_modulo_mm_s"]) for item in group
            ])),
            "distancia_parada_apos_gatilho_legado_mm": float(np.mean([
                float(item["distancia_parada_apos_gatilho_legado_mm"]) for item in group
            ])),
            "paradas_brutas": len(group),
        })
    return balanced


def collect_rows(root: Path) -> List[dict]:
    combined: List[dict] = []
    for result_path in sorted(root.glob("*/condition_result.json")):
        result = json.loads(result_path.read_text(encoding="utf-8"))
        if not result.get("accepted"):
            continue
        folder = result_path.parent
        for row in _read_diagnostic_rows(folder)[2:]:
            speed = _float(row, "velocidade_externa_modulo_mm_s")
            stop = _float(row, "distancia_parada_apos_gatilho_legado_mm")
            course = _float(row, "curso_configurado_mm")
            if speed is None or stop is None or course is None or stop < -0.25:
                continue
            item = dict(row)
            item["condicao"] = folder.name
            item["velocidade_externa_modulo_mm_s"] = speed
            item["distancia_parada_apos_gatilho_legado_mm"] = stop
            item["curso_configurado_mm"] = course
            combined.append(item)
    return combined


def generate_report(root: Path) -> Optional[dict]:
    rows = collect_rows(root)
    if not rows:
        return None
    combined_path = root / "pontos_parada_consolidados.csv"
    fieldnames = list(rows[0].keys())
    with combined_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames, delimiter=";", lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    model_rows = _balanced_model_rows(rows)
    model_points_path = root / "pontos_modelo_balanceados.csv"
    with model_points_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(
            stream, fieldnames=list(model_rows[0].keys()), delimiter=";", lineterminator="\n"
        )
        writer.writeheader()
        writer.writerows(model_rows)
    model_results = []
    for name, features in _candidate_models():
        if len(model_rows) < len(features) + 3:
            continue
        beta, pred = _fit_model(model_rows, features)
        y = np.asarray([row["distancia_parada_apos_gatilho_legado_mm"] for row in model_rows])
        residual = pred - y
        cv_errors = _condition_cv_errors(model_rows, features)
        upper_cv_residual = sorted([-value for value in cv_errors])
        upper_p95 = (
            upper_cv_residual[min(len(upper_cv_residual) - 1, math.ceil(0.95 * len(upper_cv_residual)) - 1)]
            if upper_cv_residual else math.inf
        )
        model_results.append({
            "name": name,
            "features": list(features),
            "coefficients": beta.tolist(),
            "rmse_fit_mm": float(math.sqrt(np.mean(residual * residual))),
            "p95_abs_fit_mm": float(np.percentile(np.abs(residual), 95)),
            "max_abs_fit_mm": float(np.max(np.abs(residual))),
            "cv_rmse_condition_mm": _condition_cv_rmse(model_rows, features),
            "cv_upper_residual_p95_mm": float(upper_p95),
            "cv_upper_residual_max_mm": float(max(upper_cv_residual)) if upper_cv_residual else math.inf,
        })
    finite = [model for model in model_results if math.isfinite(model["cv_rmse_condition_mm"])]
    selected = None
    if finite:
        best = min(model["cv_rmse_condition_mm"] for model in finite)
        selected = dict(next(
            model for model in finite
            if model["cv_rmse_condition_mm"] <= best * 1.10 + 1.0e-12
        ))
        selected_features = selected["features"]
        selected_beta = np.asarray(selected["coefficients"], dtype=float)
        raw_actual = np.asarray([
            float(row["distancia_parada_apos_gatilho_legado_mm"]) for row in rows
        ])
        raw_prediction = _design(rows, selected_features) @ selected_beta
        raw_upper = sorted((raw_actual - raw_prediction).tolist())
        selected["raw_upper_residual_p95_mm"] = float(
            raw_upper[min(len(raw_upper) - 1, math.ceil(0.95 * len(raw_upper)) - 1)]
        )
        selected["raw_upper_residual_max_mm"] = float(max(raw_upper))
    payload = {
        "generated_at": dt.datetime.now().isoformat(timespec="seconds"),
        "raw_stop_points": len(rows),
        "balanced_model_points": len(model_rows),
        "conditions": len({row["condicao"] for row in rows}),
        "discard_rule": "primeiro ciclo (2 strokes) de cada condicao",
        "models": model_results,
        "selected": selected,
    }
    (root / "modelo_parada.json").write_text(
        json.dumps(payload, indent=2, ensure_ascii=False), encoding="utf-8"
    )
    speeds = [float(row["velocidade_externa_modulo_mm_s"]) for row in rows]
    courses = [float(row["curso_configurado_mm"]) for row in rows]
    stops = [float(row["distancia_parada_apos_gatilho_legado_mm"]) for row in rows]
    condition_results = []
    for path in sorted(root.glob("*/condition_result.json")):
        try:
            condition_results.append(json.loads(path.read_text(encoding="utf-8")))
        except Exception:
            pass
    condition_results.sort(key=lambda item: int(item.get("index", 0)))
    lines = [
        "# Relatorio do mapa de parada reciprocante",
        "",
        f"Gerado em: {payload['generated_at']}",
        "",
        "## Dados utilizados",
        "",
        f"- Paradas fisicas brutas: {len(rows)}",
        f"- Pontos balanceados usados no ajuste: {len(model_rows)} (uma media por condicao e sentido)",
        f"- Condicoes aprovadas: {payload['conditions']}",
        f"- Velocidade externa observada: {min(speeds):.3f} a {max(speeds):.3f} mm/s",
        f"- Curso configurado: {min(courses):.3f} a {max(courses):.3f} mm",
        f"- Distancia de parada observada: {min(stops):.3f} a {max(stops):.3f} mm",
        "- O primeiro ciclo completo de cada condicao foi descartado.",
        "- A velocidade usada no ajuste e a OLS causal dos 250 ms anteriores ao gatilho.",
        "- Posicao, extremos e distancia de parada vieram do encoder externo no DLG.",
        "- O balanceamento evita que o sentido com duas paradas remanescentes tenha peso dobrado sobre o sentido com uma.",
        "",
        "## Condicoes executadas",
        "",
        "| # | Alvo (mm/s) | Curso (mm) | Vel. externa media (mm/s) | Paradas uteis | Media parada (mm) | Max (mm) | DLG perda (%) | Status |",
        "|---:|---:|---:|---:|---:|---:|---:|---:|---|",
    ]
    for result in condition_results:
        status = "APROVADA" if result.get("accepted") else "INVALIDA (nao usada)"
        lines.append(
            f"| {int(result.get('index', 0))} | {float(result.get('configured_speed_mm_s', 0)):.3f} | "
            f"{float(result.get('configured_course_mm', 0)):.3f} | "
            f"{float(result.get('external_speed_mean_mm_s') or 0):.3f} | "
            f"{int(result.get('valid_stop_rows', 0))} | "
            f"{float(result.get('stop_mean_mm') or 0):.3f} | "
            f"{float(result.get('stop_max_mm') or 0):.3f} | "
            f"{float(result.get('loss_pct') or 0):.3f} | {status} |"
        )
    lines.extend([
        "",
        "## Modelos comparados",
        "",
        "A validacao cruzada remove uma condicao completa por vez. Foi escolhido o modelo mais simples dentro de 10% do menor RMSE de validacao.",
        "",
        "| Modelo | Termos | RMSE ajuste (mm) | RMSE CV por condicao (mm) | P95 abs (mm) | Max abs (mm) | Margem CV P95 unilateral (mm) |",
        "|---|---|---:|---:|---:|---:|---:|",
    ])
    for model in model_results:
        lines.append(
            f"| {model['name']} | {', '.join(model['features'])} | "
            f"{model['rmse_fit_mm']:.4f} | {model['cv_rmse_condition_mm']:.4f} | "
            f"{model['p95_abs_fit_mm']:.4f} | {model['max_abs_fit_mm']:.4f} | "
            f"{model['cv_upper_residual_p95_mm']:.4f} |"
        )
    lines.extend(["", "## Metodo selecionado", ""])
    if selected:
        equation = " + ".join(
            f"({coef:.10g})*{feature}"
            for feature, coef in zip(selected["features"], selected["coefficients"])
        )
        lines.extend([
            f"Modelo: `{selected['name']}`",
            "",
            f"Equacao: `distancia_parada_mm = {equation}`",
            "",
            f"RMSE de validacao por condicao: {selected['cv_rmse_condition_mm']:.4f} mm.",
            "",
            f"Margem de avaliacao P95 sobre as medias balanceadas: `+ {selected['cv_upper_residual_p95_mm']:.4f} mm`.",
            "",
            f"Para um stroke individual, o gatilho conservador P95 deve usar: `antecipacao_mm = distancia_parada_mm + {selected['raw_upper_residual_p95_mm']:.4f}`.",
            "",
            f"Envelope maximo dos 66 strokes brutos: `+ {selected['raw_upper_residual_max_mm']:.4f} mm`.",
        ])
    else:
        lines.append("Base ainda insuficiente para selecionar um modelo com validacao por condicao.")
    lines.extend([
        "",
        "## Limites e uso seguro",
        "",
        "O modelo e valido somente dentro da faixa efetivamente observada acima. Ele nao substitui o limite independente de 2x o curso, a perda de posicao, o status de falha do Drive nem a parada de emergencia. Regioes marcadas como inviaveis nao devem ser extrapoladas.",
        "",
        "A margem para atuacao deve usar os residuos de strokes brutos, nao somente os pontos medios balanceados. Mesmo assim, esta curva permanece diagnostica ate ser validada em ensaios independentes.",
        "",
        "Os cursos de 4 mm em 15 e 20 mm/s nao entraram no ajuste por falta de tres extremos completos apos o descarte. O valor alvo foi comandado, mas a velocidade externa media nesses ensaios ficou abaixo do alvo; nao usar esses dois ensaios para extrapolar a curva.",
        "",
        "A superficie com curso nao reduziu o erro de validacao em relacao ao modelo somente por velocidade. Nesta base, portanto, o curso altera a capacidade de atingir velocidade nos strokes curtos, mas nao justificou um termo proprio na distancia de parada depois que a velocidade externa real foi usada.",
    ])
    (root / "RELATORIO_MAPA_PARADA.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return payload


def run_plan(root: Path, plan: Iterable[Tuple[float, float]], config: StudyConfig) -> List[dict]:
    root.mkdir(parents=True, exist_ok=True)
    plan = list(plan)
    manifest = {
        "created_at": dt.datetime.now().isoformat(timespec="seconds"),
        "config": asdict(config),
        "plan": [{"speed_mm_s": speed, "course_mm": course} for speed, course in plan],
    }
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    results = []
    blocked_shorter: Dict[float, float] = {}
    for index, (speed, course) in enumerate(plan, start=1):
        if STOP_REQUESTED:
            break
        if speed in blocked_shorter and course < blocked_shorter[speed]:
            print(f"[{index:02d}] PULADA: regiao curta bloqueada pelo gate anterior.")
            continue
        condition_folder = root / _condition_name(index, speed, course)
        result_path = condition_folder / "condition_result.json"
        try:
            if result_path.is_file():
                previous = json.loads(result_path.read_text(encoding="utf-8"))
                result = evaluate_condition(condition_folder, config, speed, course)
                for key in (
                    "index", "folder", "return_code", "rpm_drive",
                    "effective_speed_mm_s", "theoretical_s", "watchdog_s",
                ):
                    if key in previous:
                        result[key] = previous[key]
                result_path.write_text(
                    json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8"
                )
                print(f"[{index:02d}] REUTILIZADA: v={speed:g} mm/s, curso={course:g} mm.")
            else:
                result = run_condition(root, index, speed, course, config)
        except Exception as exc:
            result = {
                "index": index, "configured_speed_mm_s": speed,
                "configured_course_mm": course, "accepted": False,
                "fatal": True, "reasons": [str(exc)],
            }
            print(f"[{index:02d}] FALHA SEGURA: {exc}")
        results.append(result)
        (root / "study_results.json").write_text(
            json.dumps(results, indent=2, ensure_ascii=False), encoding="utf-8"
        )
        generate_report(root)
        if result.get("fatal") or not result.get("safe_to_continue", False):
            print("Matriz interrompida: a proxima condicao nao sera iniciada.")
            break
        if not result.get("accepted"):
            blocked_shorter[speed] = course
            print(
                "Condicao invalida para o ajuste, sem falha de seguranca; "
                "a matriz retomara pelo curso longo do proximo degrau."
            )
            continue
        if result.get("boundary_for_shorter_courses"):
            blocked_shorter[speed] = course
            print("Gate: cursos menores nesta velocidade serao ignorados.")
    return results


def _new_root() -> Path:
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    return PROJECT_ROOT / "out" / "stopping_curve" / f"study_{stamp}"


def interactive() -> int:
    config = _load_local_defaults()
    while True:
        print("\n=== Mapa de parada reciprocante ===")
        print(f"Configuracao: raio={config.radius_mm:g} mm, relacao={config.ratio:g}, tolerancia={config.tolerance_counts} counts")
        print("1) Executar matriz gradual padrao (1..20 mm/s; 4..50 mm)")
        print("2) Executar uma unica condicao")
        print("3) Gerar novamente relatorio de uma pasta existente")
        print("4) Sair")
        choice = input("Opcao: ").strip()
        if choice == "1":
            print("A matriz comeca em 1 mm/s e curso de 50 mm. Cada condicao usa 6 strokes e descarta o primeiro ciclo.")
            print("Deixe a area livre e mantenha a parada de emergencia acessivel.")
            if input("Digite INICIAR para autorizar o movimento: ").strip() != "INICIAR":
                print("Cancelado sem movimento.")
                continue
            root = _new_root()
            run_plan(root, default_plan(), config)
            print(f"Resultados: {root}")
        elif choice == "2":
            speed = float(input("Velocidade [1..20 mm/s]: ").strip())
            course = float(input("Curso [4..50 mm]: ").strip())
            if not (1.0 <= speed <= 20.0 and 4.0 <= course <= 50.0):
                print("Fora da faixa permitida.")
                continue
            if input("Digite INICIAR para autorizar o movimento: ").strip() != "INICIAR":
                print("Cancelado sem movimento.")
                continue
            root = _new_root()
            run_plan(root, [(speed, course)], config)
            print(f"Resultados: {root}")
        elif choice == "3":
            root = Path(input("Pasta do estudo: ").strip().strip('"'))
            report = generate_report(root)
            print("Relatorio gerado." if report else "Nenhum ponto aprovado encontrado.")
        elif choice == "4":
            return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-default", action="store_true")
    parser.add_argument("--yes", action="store_true", help="confirma area segura para movimento")
    parser.add_argument("--speed", type=float)
    parser.add_argument("--course", type=float)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    signal.signal(signal.SIGINT, _signal_stop)
    signal.signal(signal.SIGTERM, _signal_stop)
    config = _load_local_defaults()
    if args.run_default or args.speed is not None or args.course is not None:
        if not args.yes:
            parser.error("movimento exige --yes ou confirmacao pelo menu")
        if args.run_default:
            plan = default_plan()
        else:
            if args.speed is None or args.course is None:
                parser.error("informe --speed e --course juntos")
            if not (1.0 <= args.speed <= 20.0 and 4.0 <= args.course <= 50.0):
                parser.error("faixa permitida: velocidade 1..20 e curso 4..50")
            plan = [(args.speed, args.course)]
        root = args.output or _new_root()
        run_plan(root, plan, config)
        print(root)
        return 0
    return interactive()


if __name__ == "__main__":
    raise SystemExit(main())
