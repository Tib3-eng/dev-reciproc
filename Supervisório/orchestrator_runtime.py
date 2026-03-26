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
- Separar artefatos tecnicos em subpasta DadosDev para nao poluir a pasta REP.

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
import os
import shutil
import subprocess
import sys
import time
from itertools import zip_longest
from dataclasses import dataclass
from pathlib import Path
from typing import List, Tuple, Optional


# -------------------------------
# Configuracoes padrao (podem ser sobrescritas por quem chama o modulo)
# -------------------------------
DEFAULT_RATE_HZ = 50.0
DEFAULT_DLG_IP = "192.168.1.100"
DEFAULT_DLG_PORT = 41401
DEFAULT_BIND_IP = ""
DEFAULT_BIND_PORT = 41402


@dataclass
class RunState:
    # Handles dos subprocessos em execucao.
    dlg_proc: subprocess.Popen
    drive_proc: subprocess.Popen
    turn_proc: Optional[subprocess.Popen]
    # Caminhos de arquivos de saida do ensaio.
    dlg_csv: str
    drive_csv: str
    turn_csv: str
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
    force_normal_n: float
    relacao: float
    raio_mm: float = 0.0
    # Estado logico de pausa observado pelo orquestrador.
    paused: bool = False


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
        dict com caminhos: folder, info_csv, dlg_csv, drive_csv, turn_csv, merge_csv, schedule_csv.

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
        "drive_csv": os.path.join(folder_path, "drive.csv"),
        "turn_csv": os.path.join(folder_path, "atrito_por_volta.csv"),
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
    - merge_logs.exe

    Parametros:
        repo_root: raiz do repositorio.

    Retorna:
        dict com:
        - dlg_exe, drive_exe, merge_exe: caminhos resolvidos (ou None)
        - missing: lista de descricoes dos executaveis nao encontrados
    """
    dlg_exe = find_exe([
        os.path.join(repo_root, "DLG4000", "bin", "dlg_logger_ipc.exe"),
        os.path.join(repo_root, "DLG4000", "bin", "Release", "dlg_logger_ipc.exe"),
    ], "dlg_logger_ipc.exe")

    drive_exe = find_exe([
        os.path.join(repo_root, "DriveA5", "build", "Release", "a5_speed_logger.exe"),
        os.path.join(repo_root, "DriveA5", "build", "a5_speed_logger.exe"),
    ], "a5_speed_logger.exe")

    merge_exe = find_exe([
        os.path.join(repo_root, "DriveA5", "build", "Release", "merge_logs.exe"),
        os.path.join(repo_root, "DriveA5", "build", "merge_logs.exe"),
    ], "merge_logs.exe")

    missing = []
    if not dlg_exe:
        missing.append("dlg_logger_ipc.exe (DLG4000/bin/Release)")
    if not drive_exe:
        missing.append("a5_speed_logger.exe (DriveA5/build/Release)")
    if not merge_exe:
        missing.append("merge_logs.exe (DriveA5/build/Release)")
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
        out_paths: caminhos de saida (dlg_csv, drive_csv, turn_csv, merge_csv, schedule_csv).
        schedule: lista de etapas (rpm, duracao_s).
        duration_s: duracao total do ensaio.
        rate_hz: taxa alvo de aquisicao.
        dlg_ip/dlg_port: destino UDP do DLG.
        bind_ip/bind_port: bind local para recebimento UDP.
        com_port/slave_id/baud/parity: parametros seriais do Drive.
        show_console: quando False, tenta ocultar janela de console no Windows.
        raio_mm: raio da trilha no disco (usado para derivar velocidade media por volta).

    Retorna:
        RunState com handles de processo, caminhos e parametros da execucao.

    Excecoes:
        FileNotFoundError: quando algum executavel obrigatorio nao e encontrado.
    """
    if "turn_csv" not in out_paths or not out_paths.get("turn_csv"):
        base_folder = os.path.dirname(out_paths.get("merge_csv", "")) or os.getcwd()
        out_paths["turn_csv"] = os.path.join(base_folder, "atrito_por_volta.csv")

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
        "--ipc",
    ]
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
        "--rate", f"{rate_hz:.6f}",
        "--slave", str(slave_id),
        "--baud", str(baud),
        "--parity", parity,
        "--setup",
        "--ipc",
    ]
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

    return RunState(
        dlg_proc=dlg_proc,
        drive_proc=drive_proc,
        turn_proc=None,
        dlg_csv=out_paths["dlg_csv"],
        drive_csv=out_paths["drive_csv"],
        turn_csv=out_paths["turn_csv"],
        merge_csv=out_paths["merge_csv"],
        schedule_csv=out_paths["schedule_csv"],
        dlg_exe=dlg_exe,
        drive_exe=drive_exe,
        turn_exe="",
        merge_exe=merge_exe,
        duration_s=duration_s,
        rate_hz=rate_hz,
        force_normal_n=force_normal_n,
        relacao=relacao,
        raio_mm=raio_mm,
    )


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
    state.dlg_proc.wait()
    state.drive_proc.wait()

    # Reprocessa sempre no final para garantir consistencia do arquivo final
    # com os CSVs completos (independente de glitch no tempo real).
    _rebuild_turn_csv_from_logs(state)

    merge_rc = -1
    merge_cmd = [
        state.merge_exe,
        "--dlg", state.dlg_csv,
        "--drive", state.drive_csv,
        "--out", state.merge_csv,
    ]
    try:
        merge_rc = subprocess.call(merge_cmd)
    except Exception:
        merge_rc = -1

    # Rede de seguranca: se merge em C falhar, gera resultado em Python.
    if merge_rc != 0 or not os.path.exists(state.merge_csv):
        _merge_csv_fallback(state.dlg_csv, state.drive_csv, state.merge_csv)
        _move_dev_artifacts(state)
        return 0

    _move_dev_artifacts(state)
    return merge_rc


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
    ])

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


def _rebuild_turn_csv_from_logs(state: RunState) -> None:
    """
    Regera atrito_por_volta.csv em modo offline (sem IPC) usando logs finais.

    Esse passo roda ao final do ensaio para garantir consistencia dos dados
    por volta, mesmo se o processo em tempo real tiver encerrado cedo.
    """
    if not state:
        return
    if not os.path.exists(state.dlg_csv) or not os.path.exists(state.drive_csv):
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

    with open(state.turn_csv, "w", newline="", encoding="utf-8") as fout:
        w = csv.writer(fout, delimiter=";", lineterminator="\n")
        w.writerow([
            "volta_n", "atrito_med", "atrito_min", "atrito_max", "rpm_medio_volta", "velocidade_media_mm_s",
            "n_total_pontos", "n_falhas", "n_validas", "pct_perda"
        ])

        with open(state.dlg_csv, "r", newline="", encoding="utf-8") as fdlg, \
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
            turn_progress = 0.0
            turn_n = 1

            n_total = 0
            n_fail = 0
            n_valid = 0
            atr_sum = 0.0
            atr_min = None
            atr_max = None
            rpm_sum = 0.0
            rpm_cnt = 0

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
                if state.force_normal_n and state.force_normal_n > 0 and ch1 is not None:
                    atr = ch1 / state.force_normal_n
                atr_ok = (dlg_err == 0 and atr is not None)

                t_s_drv = _to_float(rrow[2] if rrow and len(rrow) > 2 else None, None)
                pos_err = _to_int(rrow[5] if rrow and len(rrow) > 5 else None, 1)
                rpm_err = _to_int(rrow[6] if rrow and len(rrow) > 6 else None, 1)
                pos = _to_float(rrow[3] if rrow and len(rrow) > 3 else None, None)
                rpm = _to_float(rrow[4] if rrow and len(rrow) > 4 else None, None)
                pos_mod = _to_float(rrow[7] if rrow and len(rrow) > 7 else None, 65536.0)
                if pos_mod is None or pos_mod <= 1.0:
                    pos_mod = 65536.0

                pos_ok = (pos_err == 0 and pos is not None)
                rpm_ok = (rpm_err == 0 and rpm is not None)

                n_total += 1
                if atr_ok and pos_ok:
                    n_valid += 1
                    atr_sum += atr
                    atr_min = atr if atr_min is None else min(atr_min, atr)
                    atr_max = atr if atr_max is None else max(atr_max, atr)
                    if rpm_ok:
                        rpm_sum += rpm
                        rpm_cnt += 1
                else:
                    n_fail += 1

                if pos_ok:
                    if prev_pos_valid:
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

                        # Guard rail: rejeita saltos impossiveis para evitar
                        # sobrecontagem de voltas por glitches de leitura.
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
                        # i = D2/D1 -> voltas_pino = voltas_motor / i
                        pin_turn_inc = motor_turn_inc / relacao
                        turn_progress += pin_turn_inc

                        while turn_progress >= 1.0:
                            pct_loss = (100.0 * n_fail / n_total) if n_total > 0 else 0.0
                            atr_med = (atr_sum / n_valid) if n_valid > 0 else None
                            rpm_med = (rpm_sum / rpm_cnt) if rpm_cnt > 0 else None
                            vel_med_mm_s = None
                            if rpm_med is not None and raio_mm > 0.0 and relacao > 0.0:
                                vel_med_mm_s = abs(rpm_med) * (2.0 * 3.141592653589793 * raio_mm) / (60.0 * relacao)
                            w.writerow([
                                turn_n,
                                f"{atr_med:.6f}" if atr_med is not None else "NULL",
                                f"{atr_min:.6f}" if atr_min is not None else "NULL",
                                f"{atr_max:.6f}" if atr_max is not None else "NULL",
                                f"{rpm_med:.6f}" if rpm_med is not None else "NULL",
                                f"{vel_med_mm_s:.6f}" if vel_med_mm_s is not None else "NULL",
                                n_total, n_fail, n_valid, f"{pct_loss:.3f}"
                            ])
                            turn_n += 1
                            turn_progress -= 1.0
                            n_total = 0
                            n_fail = 0
                            n_valid = 0
                            atr_sum = 0.0
                            atr_min = None
                            atr_max = None
                            rpm_sum = 0.0
                            rpm_cnt = 0

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


def stop_run(state: Optional[RunState]) -> None:
    """
    Para os dois processos com prioridade para encerramento gracioso.

    Estrategia:
    1) Envia STOP via IPC para DLG e Drive (melhor esforco).
    2) Aguarda curto periodo para flush/fechamento limpo.
    3) Se ainda vivos, aplica terminate() e depois kill() como ultimo recurso.

    Parametros:
        state: estado do ensaio em execucao.
    """
    if not state:
        return
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
