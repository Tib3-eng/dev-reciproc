"""Bounded communication diagnostics; never sends motor control commands."""
import csv
import ctypes
from ctypes import wintypes
import math
import os
from pathlib import Path
import struct
import subprocess
import time

import serial

DLG_ROUNDS = 3
DLG_DURATION_S = 3
DLG_RATE_HZ = 200
DRIVE_READS = 40


def list_process_names():
    """Enumerate executable names without WMI, elevation or command windows."""
    if os.name != "nt":
        raise RuntimeError("Verificacao de processos requer Windows.")

    class ProcessEntry(ctypes.Structure):
        _fields_ = [("size", wintypes.DWORD), ("usage", wintypes.DWORD),
                    ("pid", wintypes.DWORD), ("heap", ctypes.c_size_t),
                    ("module", wintypes.DWORD), ("threads", wintypes.DWORD),
                    ("parent", wintypes.DWORD), ("priority", wintypes.LONG),
                    ("flags", wintypes.DWORD), ("exe", wintypes.WCHAR * 260)]

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    for name in ("Process32FirstW", "Process32NextW"):
        fn = getattr(kernel, name)
        fn.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
        fn.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    handle = kernel.CreateToolhelp32Snapshot(2, 0)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        entry = ProcessEntry()
        entry.size = ctypes.sizeof(entry)
        if not kernel.Process32FirstW(handle, ctypes.byref(entry)):
            raise ctypes.WinError(ctypes.get_last_error())
        names = []
        while True:
            names.append(entry.exe)
            if not kernel.Process32NextW(handle, ctypes.byref(entry)):
                if ctypes.get_last_error() != 18:  # ERROR_NO_MORE_FILES
                    raise ctypes.WinError(ctypes.get_last_error())
                return names
    finally:
        kernel.CloseHandle(handle)


def lynx_assistants(names=None):
    names = list_process_names() if names is None else names
    # Real observed process: DLG4KAssistant.exe. Do not match generic Lynx services.
    return sorted({name for name in names
                   if name.lower().removesuffix(".exe") in
                   {"dlg4kassist", "dlg4kassistant"}})


def require_dlg_available():
    found = lynx_assistants()
    if found:
        raise RuntimeError(
            "O DLG4Kassist da Lynx esta aberto (" + ", ".join(found) + ").\n\n"
            "Ele pode enviar requisicoes ao DLG e causar perdas de dados. "
            "Feche o programa antes de iniciar o ensaio ou testar a comunicacao, "
            "e tente novamente.")


def analyze_dlg_csv(path, expected=600, rate=DLG_RATE_HZ):
    """Judge every slot, including missing tail and startup failures."""
    valid = 0
    longest = streak = rows = 0
    structure_ok = True
    with open(path, newline="", encoding="utf-8-sig") as stream:
        for index, row in enumerate(csv.DictReader(stream)):
            rows += 1
            try:
                slot_ok = (int(row["idx"]) == index
                           and abs(float(row["t_s"]) - index / rate) < 1.5 / rate)
                sample_ok = (row["err"] == "0" and slot_ok
                             and all(math.isfinite(float(row[f"ch{ch}"]))
                                     for ch in range(1, 9)))
                structure_ok = structure_ok and slot_ok
            except (KeyError, TypeError, ValueError):
                sample_ok = False
                structure_ok = False
            if sample_ok:
                valid += 1
                streak = 0
            else:
                streak += 1
                longest = max(longest, streak)
    longest = max(longest, streak + max(0, expected - rows))
    loss_pct = 100.0 * (1.0 - min(valid, expected) / expected)
    gap_ms = longest * 1000.0 / rate
    return dict(available=valid > 0, rows=rows, valid=valid, expected=expected,
                loss_pct=loss_pct, max_missing_ms=gap_ms,
                ok=(rows == expected and structure_ok and loss_pct <= 1.0 + 1e-9
                    and gap_ms < 100.0))


def check_dlg(exe, directory, progress, ip="192.168.1.100", port=41401):
    """Three independent STOP/setup/START acquisitions on the production bind."""
    rounds = []
    for number in range(1, DLG_ROUNDS + 1):
        require_dlg_available()
        progress(f"DLG: captura {number}/{DLG_ROUNDS} ({DLG_DURATION_S} s a 200 Hz)...")
        folder = Path(directory) / f"dlg_{number}"
        folder.mkdir(parents=True, exist_ok=True)
        path = folder / "dlg.csv"
        cmd = [str(exe), "--out", str(path), "--duration", str(DLG_DURATION_S),
               "--rate", str(DLG_RATE_HZ), "--ip", ip, "--port", str(port),
               "--bind-port", "41402", "--ipc"]
        # Keep stdin open until capture ends; timeout first asks for graceful STOP.
        with open(folder / "console.log", "w", encoding="utf-8") as output:
            proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=output,
                                    stderr=subprocess.STDOUT, text=True,
                                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            error = ""
            try:
                proc.stdin.write("START\n")
                proc.stdin.flush()
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                error = "timeout executando logger"
            except (OSError, ValueError) as exc:
                error = str(exc)
            finally:
                if proc.poll() is None:
                    try:
                        proc.stdin.write("STOP\n")
                        proc.stdin.flush()
                        proc.wait(timeout=2)
                    except (OSError, ValueError, subprocess.TimeoutExpired):
                        proc.kill()
                        proc.wait(timeout=2)
                proc.stdin.close()
        try:
            result = analyze_dlg_csv(path, DLG_DURATION_S * DLG_RATE_HZ)
        except (OSError, ValueError) as exc:
            result = dict(ok=False, available=False, error=str(exc))
        result.update(returncode=proc.returncode)
        result["ok"] = result["ok"] and proc.returncode == 0 and not error
        if error:
            result["error"] = error
        rounds.append(result)
        progress(f"DLG {number}/{DLG_ROUNDS}: " +
                 (f"{result['valid']}/{result['expected']} amostras, "
                  f"perda {result['loss_pct']:.2f}%, "
                  f"interrupcao {result['max_missing_ms']:.0f} ms"
                  if "valid" in result else result.get("error", "falha")))
    return dict(available=any(r["available"] for r in rounds),
                ok=all(r["ok"] for r in rounds), rounds=rounds)


def crc16(data):
    crc = 0xFFFF
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


def read_register(port, address, function=3):
    if function not in (3, 4):
        raise ValueError("Somente leituras FC03/FC04 sao permitidas.")
    body = struct.pack(">BBHH", 1, function, address, 1)
    port.reset_input_buffer()
    start = time.perf_counter()
    port.write(body + struct.pack("<H", crc16(body)))
    response = port.read(3)
    if len(response) == 3:
        # Bound the response length even if the header is corrupt.
        response += port.read(2 if response[1] & 0x80 else min(response[2], 2) + 2)
    result = dict(address=address, function=function, response=response.hex(),
                  elapsed_ms=(time.perf_counter() - start) * 1000, ok=False)
    if len(response) < 5:
        result["error"] = "timeout/short_response"
    elif crc16(response[:-2]) != int.from_bytes(response[-2:], "little"):
        result["error"] = "CRC"
    elif response[0] != 1 or response[1] not in (function, function | 0x80):
        result["error"] = "unexpected_slave/function"
    elif response[1] & 0x80:
        result["error"] = f"modbus_exception_{response[2]}"
    elif len(response) != 7 or response[2] != 2:
        result["error"] = "unexpected_length"
    else:
        result.update(ok=True, value=int.from_bytes(response[3:5], "big"))
    return result


def analyze_drive(reads):
    good = [r["elapsed_ms"] for r in reads if r["ok"]]
    mean = sum(good) / len(good) if good else None
    p95 = sorted(good)[math.ceil(.95 * len(good)) - 1] if good else None
    maximum = max(good) if good else None
    return dict(available=bool(good), successful=len(good), attempted=len(reads),
                mean_ms=mean, p95_ms=p95, max_ms=maximum,
                ok=(len(good) == DRIVE_READS and len(reads) == DRIVE_READS
                    and mean <= 20 and p95 <= 50 and maximum <= 100))


def check_drive(progress, port="COM5"):
    reads, probes = [], []
    addresses = (0x0603, 0x3100)  # RPM setpoint and VDI; read-only throughout.
    functions = {}
    with serial.Serial(port, 115200, bytesize=8, parity="E", stopbits=1,
                       timeout=.2, write_timeout=.2) as connection:
        for address in addresses:
            for fc in (3, 4):
                result = read_register(connection, address, fc)
                probes.append(result)
                time.sleep(.025)
                if result["ok"]:
                    functions[address] = fc
                    break
        for number in range(DRIVE_READS):
            address = addresses[number % 2]
            result = read_register(connection, address, functions.get(address, 3))
            reads.append(result)
            progress(f"Drive: leitura {number + 1}/{DRIVE_READS} - " +
                     (f"{result['elapsed_ms']:.1f} ms" if result["ok"] else result["error"]))
            time.sleep(.025)
    summary = analyze_drive(reads)
    # Function discovery may fall back on a valid unsupported-function exception,
    # but transport failures in preflight must not disappear behind retries.
    summary["ok"] = summary["ok"] and all(
        p["ok"] or p.get("error") == "modbus_exception_1" for p in probes)
    return dict(**summary, reads=reads, probes=probes)


def run_check(exe, directory, progress, ip="192.168.1.100", port=41401):
    report = {"criteria": {"dlg_rounds": DLG_ROUNDS, "dlg_rate_hz": DLG_RATE_HZ,
                          "dlg_duration_s": DLG_DURATION_S, "dlg_loss_pct_max": 1,
                          "dlg_missing_ms_below": 100, "drive_reads": DRIVE_READS,
                          "drive_success_pct": 100, "drive_mean_ms_max": 20,
                          "drive_p95_ms_max": 50, "drive_max_ms": 100}}
    for device, check in (("dlg", lambda: check_dlg(exe, directory, progress, ip, port)),
                          ("drive", lambda: check_drive(progress))):
        try:
            report[device] = check()
        except Exception as exc:
            report[device] = dict(ok=False, available=False, error=str(exc))
            progress(f"{device.upper()}: {exc}")
    try:
        require_dlg_available()
    except Exception as exc:
        report["interference"] = str(exc)
    report["ok"] = (report["dlg"]["ok"] and report["drive"]["ok"]
                    and not report.get("interference"))
    return report


def format_report(report):
    lines = ["Comunicação: " + ("OK" if report["ok"] else "REPROVADA")]
    for number, r in enumerate(report["dlg"].get("rounds", []), 1):
        lines.append(f"DLG {number}/3: " + ("OK" if r["ok"] else "FALHA") +
                     (f" — perda {r['loss_pct']:.2f}%; interrupção {r['max_missing_ms']:.0f} ms"
                      if "loss_pct" in r else "") +
                     (f" ({r['error']})" if r.get("error") else ""))
    d = report["drive"]
    if "successful" in d:
        lines.append("Drive: " + ("OK" if d["ok"] else "FALHA") +
                     f" — {d['successful']}/{DRIVE_READS} leituras válidas")
        if d["successful"]:
            lines.append(f"Tempo de resposta: média {d['mean_ms']:.1f} ms; "
                         f"P95 {d['p95_ms']:.1f} ms; máximo {d['max_ms']:.1f} ms")
        failures = sorted({p.get("error", "") for p in d["probes"] + d["reads"] if not p["ok"]})
        if failures:
            lines.append("Diagnóstico Drive: " + ", ".join(failures))
    for device in ("dlg", "drive"):
        if report[device].get("error"):
            lines.append(f"{device.upper()}: {report[device]['error']}")
    if report.get("interference") and report["interference"] != report["dlg"].get("error"):
        lines.append(report["interference"])
    lines.append("\nCritérios: DLG com perda ≤1% em cada captura e sem interrupção "
                 "de 100 ms; Drive com 40/40 leituras válidas, média ≤20 ms, "
                 "P95 ≤50 ms e máximo ≤100 ms.\n"
                 "O resultado descreve a comunicação durante este check, com motor parado.")
    return "\n".join(lines)
