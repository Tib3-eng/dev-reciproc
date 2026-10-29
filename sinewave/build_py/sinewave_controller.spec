# -*- mode: python ; coding: utf-8 -*-
# Distribuicao separada: atualizar este executavel nao exige embutir o
# algoritmo do colega no supervisorio. Ambos compartilham contrato v1.
from pathlib import Path
repo = Path(SPECPATH).resolve().parents[1]
a = Analysis([str(repo/'sinewave/sinewave_runtime.py')],
             pathex=[str(repo/'sinewave'),str(repo/'Supervis\u00f3rio')],
             binaries=[], datas=[], hiddenimports=['motor','curve_program','sinewave_protocol'],
             hookspath=[], hooksconfig={}, runtime_hooks=[], excludes=[], noarchive=False)
pyz = PYZ(a.pure)
exe = EXE(pyz,a.scripts,a.binaries,a.datas,[],name='sinewave_controller',
          debug=False,bootloader_ignore_signals=False,strip=False,upx=True,
          console=True,disable_windowed_traceback=False)
