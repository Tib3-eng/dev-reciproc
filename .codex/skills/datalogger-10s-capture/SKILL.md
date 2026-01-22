---
name: datalogger-10s-capture
description: Compilar e rodar o DLGlogger por ~10 s a 200 Hz; validar ~2000 amostras.
metadata:
  short-description: Captura bruta e checagem de contagem de linhas.
---
Objetivo:
- Compilar DLG4000 (Release).
- Executar DLGlogger.exe e aguardar arquivo CSV de saida (ex.: dados_ch1.csv).
- Validar ~2000 linhas (10 s * 200 Hz). Se faltar, sugerir aumentar o START_WAIT_MS.

Passos:
1) Build:
   cmake -S DLG4000 -B DLG4000/build -G "Visual Studio 17 2022" -A x64
   cmake --build DLG4000/build --config Release

2) Run:
   DLG4000/build/Release/DLGlogger.exe

3) Validar:
   - Checar linhas uteis no CSV.
   - Imprimir ?Samples saved? e ?Loss events? se o binario reportar.
   - Comentar se ha ?startup lag? e recomendar ajuste em codigo se necessario.

Restri??es:
- Nao alterar structs do protocolo sem diff claro.
- Nao gravar fora do workspace.
