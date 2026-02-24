# -*- mode: python ; coding: utf-8 -*-
import os
import sys
from PyInstaller.utils.hooks import collect_all

datas = []
binaries = []
hiddenimports = []
tmp_ret = collect_all('matplotlib')
datas += tmp_ret[0]; binaries += tmp_ret[1]; hiddenimports += tmp_ret[2]
tmp_ret = collect_all('openpyxl')
datas += tmp_ret[0]; binaries += tmp_ret[1]; hiddenimports += tmp_ret[2]

SPEC_PATH = os.path.abspath(sys.argv[0]) if sys.argv and sys.argv[0].lower().endswith(".spec") else os.path.abspath("novo_tribometro.spec")
SPEC_DIR = os.path.dirname(SPEC_PATH)
REPO_ROOT = os.path.abspath(os.path.join(SPEC_DIR, '..', '..'))
ICON_PATH = os.path.join(REPO_ROOT, 'assets', 'logo.ico')
PNG_PATH = os.path.join(REPO_ROOT, 'assets', 'logo.png')
if os.path.exists(ICON_PATH):
    datas += [(ICON_PATH, '.')]
if os.path.exists(PNG_PATH):
    datas += [(PNG_PATH, '.')]


a = Analysis(
    ['..\\novo_tribometro.py'],
    pathex=[],
    binaries=binaries,
    datas=datas,
    hiddenimports=hiddenimports,
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
    name='novo_tribometro',
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
    icon=[ICON_PATH],
)
