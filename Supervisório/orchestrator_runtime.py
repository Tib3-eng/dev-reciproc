"""
orchestrator_runtime.py
-----------------------
Small, focused orchestrator used by novo_tribometro.py.

Goals:
- Keep novo_tribometro.py changes minimal.
- Start/stop headless C loggers (DLG + Drive) and merge outputs.
- Create deterministic file structure in Desktop\\Repositorio.

Important:
- This module avoids any UI work. It only manages files/processes.
- All paths are explicit to keep behavior predictable and debuggable.
"""

import csv
import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import List, Tuple, Optional


# -------------------------------
# Configuration defaults (can be overridden by caller)
# -------------------------------
DEFAULT_RATE_HZ = 200.0
DEFAULT_DLG_IP = "192.168.1.100"
DEFAULT_DLG_PORT = 41401
DEFAULT_BIND_IP = ""
DEFAULT_BIND_PORT = 41402


@dataclass
class RunState:
    # Process handles
    dlg_proc: subprocess.Popen
    drive_proc: subprocess.Popen
    # File paths
    dlg_csv: str
    drive_csv: str
    merge_csv: str
    schedule_csv: str
    # Executables (for debugging)
    dlg_exe: str
    drive_exe: str
    merge_exe: str
    # Run settings
    duration_s: float
    rate_hz: float


def sanitize_folder_name(name: str) -> str:
    """
    Windows-safe folder name:
    - replace reserved characters with '_'
    - trim trailing dots/spaces (Windows disallows)
    """
    invalid = '<>:"/\\\\|?*'
    out = ''.join('_' if c in invalid else c for c in name)
    out = out.rstrip(' .')
    return out if out else "Ensaio"


def build_output_paths(base_dir: str, nome_ensaio: str, estudo: str) -> dict:
    """
    Build the standardized output folder and file names.
    Raises FileExistsError if the folder already exists.
    """
    folder_name = sanitize_folder_name(f"{nome_ensaio} - {estudo}")
    folder_path = os.path.join(base_dir, folder_name)
    if os.path.exists(folder_path):
        raise FileExistsError(folder_path)
    os.makedirs(folder_path, exist_ok=True)

    return {
        "folder": folder_path,
        "info_csv": os.path.join(folder_path, "info.csv"),
        "dlg_csv": os.path.join(folder_path, "dlg.csv"),
        "drive_csv": os.path.join(folder_path, "drive.csv"),
        "merge_csv": os.path.join(folder_path, "merge.csv"),
        "schedule_csv": os.path.join(folder_path, "schedule.csv"),
    }


def write_schedule_csv(path: str, schedule: List[Tuple[int, float]]) -> None:
    """
    schedule: list of (rpm, duration_s)
    """
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["rpm", "duration_s"])
        for rpm, dur_s in schedule:
            w.writerow([rpm, f"{dur_s:.6f}"])


def rpm_from_mm_s(vel_mm_s: float, raio_mm: float) -> int:
    """
    Convert linear speed (mm/s) to RPM using radius in mm.
    rpm = v * 60 / (2*pi*raio)
    """
    if raio_mm <= 0:
        return 0
    rpm = (vel_mm_s * 60.0) / (2.0 * 3.141592653589793 * raio_mm)
    # Round to nearest int (Drive expects int16)
    return int(rpm + 0.5 if rpm >= 0 else rpm - 0.5)


def find_exe(candidates: List[str], fallback_name: Optional[str] = None) -> Optional[str]:
    """
    Return the first existing executable among candidates,
    otherwise try PATH (fallback_name).
    """
    for c in candidates:
        if c and os.path.exists(c):
            return c
    if fallback_name:
        return shutil.which(fallback_name)
    return None


def find_repo_root(start: Optional[Path] = None) -> str:
    """
    Try to locate the repo root by walking upwards and finding DLG4000 + DriveA5.
    This is robust for both source runs and PyInstaller (sys.frozen).
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
    # Fallback to current working directory
    return os.getcwd()


def check_executables(repo_root: str) -> dict:
    """
    Locate required executables and report missing ones.
    Returns dict with keys: dlg_exe, drive_exe, merge_exe, missing (list).
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
        "merge_exe": merge_exe,
        "missing": missing,
    }


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
    com_port: str = "COM4",
    slave_id: int = 1,
    baud: int = 115200,
    parity: str = "E",
    show_console: bool = False,
) -> RunState:
    """
    Launch DLG logger + Drive logger (headless).
    - Uses --ipc: each process prints READY and waits for START on stdin.
    - Writes schedule.csv before launching.
    """
    write_schedule_csv(out_paths["schedule_csv"], schedule)

    # Locate executables (relative to repo root)
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

    # Launch DLG logger (headless)
    dlg_cmd = [
        dlg_exe,
        "--out", out_paths["dlg_csv"],
        "--duration", f"{duration_s:.6f}",
        "--rate", f"{rate_hz:.6f}",
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

    # Launch Drive logger (headless)
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

    # Wait for READY (best-effort, short timeout)
    _wait_ready(dlg_proc, "DLG")
    _wait_ready(drive_proc, "Drive")

    # Start DLG first, wait for first data, then start Drive
    _send_start(dlg_proc)
    _wait_data_ready(dlg_proc, timeout_s=6.0)
    _send_start(drive_proc)

    return RunState(
        dlg_proc=dlg_proc,
        drive_proc=drive_proc,
        dlg_csv=out_paths["dlg_csv"],
        drive_csv=out_paths["drive_csv"],
        merge_csv=out_paths["merge_csv"],
        schedule_csv=out_paths["schedule_csv"],
        dlg_exe=dlg_exe,
        drive_exe=drive_exe,
        merge_exe=merge_exe,
        duration_s=duration_s,
        rate_hz=rate_hz,
    )


def wait_and_merge(state: RunState) -> int:
    """
    Wait for both loggers to finish and run merge tool.
    Returns merge exit code.
    """
    state.dlg_proc.wait()
    state.drive_proc.wait()

    merge_cmd = [
        state.merge_exe,
        "--dlg", state.dlg_csv,
        "--drive", state.drive_csv,
        "--out", state.merge_csv,
    ]
    return subprocess.call(merge_cmd)


def stop_run(state: Optional[RunState]) -> None:
    """
    Hard stop for both processes.
    """
    if not state:
        return
    for p in (state.dlg_proc, state.drive_proc):
        if p and p.poll() is None:
            try:
                p.terminate()
            except Exception:
                pass


def _wait_ready(proc: subprocess.Popen, tag: str, timeout_s: float = 5.0) -> None:
    """
    Best-effort wait for 'READY' line from the logger.
    If nothing arrives, we continue (to avoid deadlock).
    """
    if not proc or not proc.stdout:
        return
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        line = proc.stdout.readline()
        if not line:
            time.sleep(0.05)
            continue
        if "READY" in line:
            return


def _send_start(proc: subprocess.Popen) -> None:
    if not proc or not proc.stdin:
        return
    try:
        proc.stdin.write("START\n")
        proc.stdin.flush()
    except Exception:
        pass


def _wait_data_ready(proc: subprocess.Popen, timeout_s: float = 5.0) -> bool:
    """
    Wait for DLG logger to confirm first data sample.
    Expected lines: DATA_OK or DATA_TIMEOUT.
    Returns True if DATA_OK, False otherwise.
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
