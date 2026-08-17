# -*- mode: python ; coding: utf-8 -*-

from pathlib import Path

repo_root = Path(SPECPATH).resolve().parent

a = Analysis(
    [str(repo_root / "MapeiaParadaReciprocante" / "mapa_parada.py")],
    pathex=[str(repo_root / "Supervisório")],
    binaries=[
        (str(repo_root / "DLG4000" / "bin" / "Release" / "dlg_logger_ipc.exe"), "DLG4000/bin/Release"),
        (str(repo_root / "DriveA5" / "build_vs2022" / "Release" / "a5_speed_logger.exe"), "DriveA5/build_vs2022/Release"),
        (str(repo_root / "DriveA5" / "build_vs2022" / "Release" / "merge_logs.exe"), "DriveA5/build_vs2022/Release"),
        (str(repo_root / "DriveA5" / "build_vs2022" / "Release" / "modbus-5.dll"), "DriveA5/build_vs2022/Release"),
    ],
    datas=[],
    hiddenimports=[],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name="mapa_parada_reciprocante",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=True,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
