---
name: drivea5-quick-check
description: Compilar a CLI do DriveA5 e executar leitura basica de posicao/velocidade.
metadata:
  short-description: Build + leitura essencial sem comandar movimento.
---
Objetivo:
- Compilar DriveA5 (Release).
- Rodar a5_cli.exe em modo somente leitura (nao enviar jog/torque).
- Imprimir sane checks (porta/baud, resposta do drive).

Passos:
1) Build:
   cmake -S DriveA5 -B DriveA5/build -G "Visual Studio 17 2022" -A x64
   cmake --build DriveA5/build --config Release

2) Run (exemplos):
   DriveA5/build/Release/a5_cli.exe --port COM3 --baud 115200 --status

Restri??es:
- Confirmar porta/baud antes de conectar.
- Nao enviar setpoints sem instrucao explicita.
