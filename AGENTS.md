# AGENTS.md - DevReciproc (DLG4000 + CalibraDLG + DriveA5)

Purpose
This file is only for Codex. Keep it short, actionable, ASCII-only, and optimized for editing and safety.

Project goal
Build a reciprocating tribometer with three C executables:
1) DLG4000 data acquisition (UDP).
2) CalibraDLG calibration tool (UDP).
3) DriveA5 motor control + position logging (Modbus RTU).
CalibraDLG_UI (WinForms) is the calibration UI for CalibraDLG.
Python UI is an orchestrator/visualizer (not the only UI).
Current status: DLG4000 and DriveA5 projects are working and should be treated as reference implementations.

Architecture summary
- Core in C for timing control and acquisition. UIs: Python orchestrator/plotting + CalibraDLG_UI (WinForms) for calibration.
- IPC: JSON lines over STDIN/STDOUT (simple, versioned).
- Data: CSV with fixed columns and clear units (auto-described header when possible).

Key decisions (do not regress)
- Position per revolution uses P0B-09 (0..65535). Drop old heuristics.
- DLG timing uses QPC and starts only after first OP_ACQDATA to avoid startup lag.
- Explicit loss handling: inject "NULL" rows for timeouts/gaps and report counters.
- Separate timeouts: command ops are lenient, sampling is short.
- Logs are ASCII-only.

Module notes
DLG4000 (UDP/WinSock2):
- Pipeline: ACQSTOP (preventive) -> setup -> ACQSTART -> receive -> ACQSTOP.
- Accept older ACQSETUP_R (short response).
- CSV: include elapsed time, frame, raw channels; inject NULL on losses.
- Channel config uses index values from the manual: iGain index 0..7 => [1,3,10,30,100,300,1000,3000].
- Excitation uses iSensPwr index 0..4 => [1V, 2.5V, 3.3V, 5V, user].
- Current default front-end for DLG tools: iGain index 5 (300) and iSensPwr index 2 (3.3V).
- Channels are numbered 1..8 (not 0..7).
- DLGlogger and CalibraDLG are a paired workflow: CalibraDLG writes calibration JSON, DLGlogger consumes it.
- DLGlogger calibration search: calib.json, calib, calib_CHn.json, out/calib_CHn.json.
- Channel config should behave consistently across all channels; CH1 is the current read path and future versions will read other channels.
- ACQDATA samples are interleaved by burst; total samples = nSignals * nBurstsPack.
- CalibraDLG capture uses short recv timeout and can reissue ACQSETUP/START if no data arrives.
- CalibraDLG re-syncs the stream at each calibration point (drains socket and restarts ACQ).
- CalibraDLG stops/starts acquisition per point to avoid stream stalls between user inputs.
- CalibraDLG and DLGlogger are not run simultaneously. CalibraDLG writes calibration data to disk; DLGlogger loads and applies it on startup and keeps using it until recalibrated.
- Bug history: calibration stalled after point 1/2 because wait_first_packet always re-sent ACQSETUP with channel 1, and the stream could stall during long user input; fixed by using the selected channel and restarting ACQ per point with socket drain.
- dlg_logger_ipc: headless logger (8 canais) com CSV fixo (idx,t_qpc,t_s,ch1..ch8,atrito,err). Usa --ipc e espera "START" via stdin.
- dlg_logger_ipc aceita `--force-normal <N>` e calcula `atrito = ch1/forca_normal`; sem forca valida, grava NULL.
- dlg_logger_ipc grava numero fixo de linhas (duracao * taxa) e injeta NULL quando faltar amostra.
- dlg_logger_ipc aplica calibracao por canal quando calib.json/calib_CHn.json estiver presente; se nao houver, loga bruto.
- dlg_logger_ipc espera 3 amostras validas (DATA_OK) antes de iniciar o tempo; o supervisÃ³rio so inicia o Drive depois disso.
- dlg_logger_ipc --ipc agora aceita PAUSE/RESUME/STOP. Em pause ele drena e descarta pacotes, congela o tempo local e retoma sem criar slots de catch-up.
- dlg_logger_ipc escreve `dlg_logger_events.log` ao lado do `dlg.csv` com startup, READY/START, DATA_OK/TIMEOUT, pause/resume, progresso e encerramento.

CalibraDLG (UDP/WinSock2):
- Modes: interactive console (default) or --ipc (JSON lines over STDIN/STDOUT).
- IPC ops: config/point/finish/cancel; emits point_result/done/error.
- JSON output path can be set by IPC; defaults to out/calib.json.
- Runtime accepts optional `tc_cjc_mode` in `config` for termopar (0=internal, 1=external/TEDs).
- Runtime writes termopar sidecar metadata as `calib_CHn_tcmeta.json` without changing `calib_CHn.json` schema.

CalibraDLG_UI (WinForms):
- UI labels/messages are PT-BR (ASCII) and sensor modes use "meia ponte" and "ponte 1/4".
- Legend button + tooltips provide detailed explanations (including LPF as low-pass filter index).
- Table headers have tooltips (Indice/Referencia/Bruto/Amostras) and legend entries mirror them.
- Capture button sits next to "Valor de referencia" field; Start comes after Capture.
- CalibraDLG_UI resolve automaticamente o caminho do CalibraDLG.exe (sem campo de selecao manual do executavel).
- CalibraDLG_UI direciona a saida da calibracao para a pasta do dlg_logger_ipc.exe (calib_CHn.json) para aplicacao automatica nos ensaios seguintes.
- UI shows running error (RMSE) computed from current points (raw->ref linear fit).
- RMSE label shows percent relative to reference span; Finish button now reports status if process is missing or point pending.
- Finish/Cancel set "Finalizando/Cancelando" and status auto-resets on process end if no final message arrives.
- Runtime path is auto-resolved; executable path picker was removed.
- UI includes a small runtime log and "Check DLG" button to validate CalibraDLG.exe + DLG config handshake.
- UI exposes "Junta fria (termopar)" in a discrete way and keeps it disabled for non-termopar sensors.
- UI sends `tc_cjc_mode` only for termopar, preserving legacy IPC payloads for other sensors.
- UI aplica preset automatico ao selecionar termopar K: iGain=6 (1000), iLPF=1 (UI index "1"), iSensPwr=0 (1.0V), junta fria interna.

SupervisÃ³rio (Python/Tk):
- novo_tribometro.py is the main UI; novo_tribometro.exe is the packaged app.
- orchestrator_runtime.py launches dlg_logger_ipc + a5_speed_logger in background and merges logs with merge_logs.
- conversao mm/s->rpm no orchestrator usa modulo (abs) para enviar setpoint sempre positivo no modo velocidade.
- UI mostra "Velocidade alvo atual" (mm/s) e "RPM Alvo atual" (setpoint enviado ao Drive).
- A aba principal inclui bit "Monitoramento": ativa leitura DLG (CH1/CH2) sem ensaio, com janela fixa de 1 minuto nos graficos 1/2.
- Com monitoramento ativo, o botao Iniciar fica desabilitado; nao e permitido ativar monitoramento durante ensaio/tara.
- A UI nao usa mais aba de log; mensagens seguem para console e status visuais.
- Supervisório grava `graph_events.log` por ensaio com diagnostico de confiabilidade dos graficos (DLG tail, voltas por idx, gaps, extremos).
- A aba "configuracoes adicionais" permite escolher diretorio base fixo dos ensaios; valor persistido em settings local (APPDATA).
- A aba "configuracoes adicionais" inclui campo persistente "Relacao mecanica (i = D2/D1)"; D1=polia do motor, D2=polia do disco. RPM alvo usa (i * v * 60) / (2 * pi * raio).
- Tabela da UI: `Voltas_pin` = distancia/(2*pi*raio) e `Voltas_mot` = i * Voltas_pin.
- A aba "configuracoes adicionais" inclui botao "Configurar canais" para abrir CalibraDLG_UI diretamente, com bloqueio se ensaio/processos estiverem ativos.
- Botao "Configurar Eixos Y (Min / Max)" abre popup compacto com 2 linhas (Temperatura/Grafico 1 e Atrito-forca/Grafico 2), com modo Automatico (padrao) e campos Min/Max manuais.
- Botao "Configurar eixo X" abre popup com 2 blocos: (1) Temperatura+CoF com modo Automatico (duracao total prevista) ou Manual (janela em minutos com varredura ciclica), e (2) Grafico 3 com Automatico (distancia total prevista) ou Manual (janela em mm com varredura ciclica).
- A UI inclui aba "graficos" para visualizacao 2x2 em tempo real: Temperatura + CoF (esquerda) e processamento por volta + por distancia (direita), reutilizando as mesmas configuracoes de eixo X/Y da aba principal.
- Na aba "graficos", os dois paineis da direita (volta/distancia) exibem atrito no eixo esquerdo e velocidade media (mm/s) no eixo direito.
- Ao abrir a aba "graficos", a UI entra em modo foco de desempenho: colapsa o painel lateral de graficos da aba principal (sem desmontar layout com pack_forget); ao sair da aba, restaura a largura do painel sem perder continuidade dos dados.
- Renderizacao dos graficos usa um unico agendador Tk: redesenha somente a vista ativa (coluna lateral ou aba 2x2) e aguarda curto periodo apos troca de aba antes de chamar Matplotlib.
- Renderizacao dos graficos na UI aplica decimacao apenas para exibicao (downsample visual), mantendo dados completos para processamento/arquivo.
- Alternar eixo X entre manual/automatico durante ensaio preserva historico completo dos graficos 1/2; ao voltar para automatico, a serie volta a mostrar desde o inicio do ensaio.
- Grafico 2 exibe CoF (CH1/Forca normal) com legenda "CoF" e eixo Y "CoF [-]".
- A aba "configuracoes adicionais" inclui campo persistente "Tamanho do intervalo (mm)" para agregacao do grafico 3 e arquivo _P.
- Grafico 3 usa eixo X por distancia acumulada do pino (mm), derivada de `P0B-09` + `i = D2/D1` + raio.
- Grafico 3 (atrito por distancia) e calculado em tempo real no supervisÃ³rio a partir de `dlg.csv + drive.csv` (sem stream TURN em C).
- Grafico 3 plota tambem `Velocidade media` (mm/s) em eixo Y secundario, convertida de RPM medio do motor por intervalo/volta com `i = D2/D1` e raio do pino.
- Tail dos CSVs em tempo real (DLG/turnos) usa refresh de EOF no Windows para evitar congelar atualizacao ate o fim do ensaio.
- Tail dos CSVs em tempo real deve consumir apenas linhas completas (terminadas em '\\n') para evitar parse de linha parcial durante append.
- Tail threads do supervisÃ³rio usam token de ensaio para evitar duplicacao de pontos entre execucoes consecutivas.
- start_external_run agora exige READY de DLG/Drive; se algum processo nao responder, o ensaio aborta e encerra subprocessos.
- start_external_run nao aborta apenas por `DATA_TIMEOUT` inicial do DLG; o cronometro so inicia com amostra valida e o supervisÃ³rio decide abortar por timeout de validacao.
- start_external_run no supervisÃ³rio usa bind UDP fixo (41402) no logger DLG; evitar bind efemero no pipeline principal para reduzir casos de ensaio sem ACQDATA.
- Se o cronometro nao iniciar por falta de amostras validas do DLG no tempo limite, o supervisÃ³rio aborta o ensaio externo e encerra subprocessos (evita corrida com dlg.csv todo em erro).
- Taxa padrao de aquisicao no pipeline externo (DLG + Drive) ajustada para 50 Hz; manter ambas iguais para sincronismo.
- wait_and_merge has fallback merge in Python if merge_logs fails or resultado_ensaio.csv is missing.
- wait_and_merge reconstrói o arquivo _P por distancia em Python ao final de todo ensaio para garantir consistencia do arquivo final com os CSVs completos.
- Rebuild offline em Python usa a mesma regra de wrap/backstep do C (sem `%` direto no delta) para evitar sobrecontagem de voltas por jitter.
- Outputs go to Desktop\\Repositorio\\<AAAA-MM-DD - PoD - NomeEnsaio_Estudo-Repeticao> with arquivos finais nomeados por padrao:
  <data>-<nome_ensaio>-<estudo>-<repeticao>_I.csv (info), _P.csv (atrito por distancia), _T.csv (resultado ensaio).
- info_ensaio.csv inclui bloco "Dados de calibracao" com fit do CH1 (slope, intercept, r2) usando a mesma busca do dlg_logger_ipc (calib.json/calib/calib_CH1.json/out + pasta do exe).
- Artefatos tecnicos finais ficam em Desktop\\Repositorio\\<...>\\DadosDev\\:
  <arquivo_t>.merge_source, dlg.csv, drive.csv, schedule.csv, graph_events.log, dlg_logger_events.log, a5_speed_events.log.
- Move de dlg.csv/drive.csv para DadosDev e feito em duas passadas (wait_and_merge + finalize_pos-run) para evitar copia sem recorte quando houver lock temporario no Windows.
- Duplicidade de pasta e bloqueio de inicio consideram data + estudo + nome + repeticao na pasta unica da execucao.
- Se a pasta da execucao ja existir, o supervisÃ³rio pergunta se deve sobrescrever; ao confirmar duas vezes, apaga a pasta existente e reutiliza a mesma repeticao.
- abrir_configurar_canais resolves CalibraDLG_UI.exe automatically via orchestrator runtime helpers.
- Check status: DLG uses UDP ACQSTOP/SETCH/SETUP/START (8 canais) and waits for ACQDATA (no ICMP ping).
- Supervisorio (start_external_run + check status) usa COM5 como porta padrao atual do Drive.
- Check status bind: tenta 41402 (mesma do logger); se falhar, usa porta efemera e registra no log.
- Pending: If DLG check still fails, suspect DLG busy in another app or firewall/route issues.
- stop_run: envia STOP via stdin (IPC) e espera curto periodo antes de terminate/kill.
- stop_run no supervisÃ³rio: ao clicar Parar, estado vira "Finalizando...", envia STOP para ambos, aguarda fechamento e mantem merge final ate salvar arquivos parciais.
- orchestrator_runtime tem pause_run/resume_run e envia PAUSE/RESUME para ambos os processos IPC.
- Botao Pausar no supervisÃ³rio externo alterna para Retomar, congela o cronometro e o estado; ao retomar, continua da mesma etapa.
- Botao "Zerar celula" no supervisÃ³rio: coleta CH1 por 30 s (DLG em repouso), calcula media valida e ajusta `fit.intercept` do `calib_CH1.json` para tara (`novo = antigo - media`); durante a coleta o estado mostra "Coletando dados para tara".
- Tara no supervisÃ³rio preserva `fit.slope` (nao altera inclinacao) e ajusta somente `fit.intercept`; grava debug com slope/intercept antes/depois.
- Inicio de ensaio executa tara automatica obrigatoria com dois popups de confirmacao operacional (sem carga para zerar, depois pronto para iniciar).
- Log da tara automatica fica em `<pasta do ensaio>\\DadosDev\\zero_ensaio.csv`; tara manual (botao "Zerar celula") grava em `<REPO_BASE>\\ZeroAvulso\\zero_avulso_<timestamp>.csv`.
- Busca do arquivo de calibracao CH1 prioriza `calib_CH1.json`/`out\\calib_CH1.json`; arquivos genericos (`calib.json`/`calib`) so sao aceitos quando o payload indicar CH1.
- novo_tribometro captures run state snapshot to avoid race with global external_run_state.

DriveA5 (Modbus RTU / libmodbus):
- a5_cli: RUN/STOP, set RPM (P06-03), read P0B-09. Try FC03, fallback FC04.
- a5_pos_cli: position command via internal multi-segment (P05-00=2, P11-12) and VDI (P31-00).
- a5_pos_cli opens an interactive console when no args are provided (PT-BR prompts).
- a5_pos_cli auto-runs a comms probe on startup and defaults to COM4 in interactive mode.
- a5_pos_cli has --diag to read P0C/P0B plus position/VDI config and warn on mismatches; it prints DI status, command/deviation counters, and fault codes (P0B-33/34).
- a5_pos_cli keeps PosInSen level when P17-03=0 (VDI2 logic) and watches P0B-07/P0B-13/P0B-15 with a verify window (tol/timeout); it auto-expands timeout based on P05-02 and speed when needed.
- VDI mapping for position control uses P17 (VDI1=S-ON, VDI2=PosInSen) per position-parameter doc; P11-00 is set to 2 (DI switching).
- a5_pos_cli logs each parameter write (with readback) before RUN to help diagnose write order issues.
- a5_pos_cli forces P31-00=0 (STOP) before parameter writes for consistent quick-mode behavior.
- a5_pos_cli retries parameter writes (with readback fallback) to handle intermittent Modbus errors.
- a5_pos_cli has oscillation mode (--osc or interactive) that alternates 0 and 15000 with cycles/dwell.
- a5_pos_cli has "Testa posicao" mode that does one full parameter write, then updates only P11-12 for subsequent positions (with STOP + VDI re-trigger).
- a5_pos_cli has "zerar agora" (P05-30=6) to set current position as home; if that fails it falls back to a software zero offset.
- a5_pos_cli caches P0C-26 word order to avoid bad 32-bit reads when P0C-26 read fails.
- a5_pos_cli logs position checks to out/a5_pos_log.csv (raw + logical P0B-07, error, dev, cmd) and caches P05-02 (units/rev) when readable.
- Standard test: 10 rpm, 120 s, ~200 Hz. CSV: t_s,pos,rev.
- Revolution count: detect robust wrap (prev > 60000 and pos < 5000).
- a5_speed_logger: headless logger para modo velocidade (RPM) com schedule CSV (rpm,duration_s). Loga P0B-09 (posicao) e P0B-00 (actual motor speed) em 50 Hz: idx,t_qpc,t_s,pos,rpm,pos_err,rpm_err,pos_mod. Se P05-02 estiver disponivel, escala posicao para 0..(P05-02-1); caso contrario usa bruto 0..65535.
- a5_speed_logger --setup: escreve P02-00=0, P06-00=0, P06-01=3, P06-02=0, P03-02=0, P0C-09=1 e P31-00=0 para usar P06-03 como unica fonte de velocidade (evita offset por A+B).
- a5_speed_logger fim de ensaio: envia parada imediata reforcada (RPM=0 + CTRL RDY + P31-00=0 com retry curto).
- a5_speed_logger usa deadline real (QPC/wall-time) para disparar STOP no tempo alvo, mesmo se o loop de aquisicao estiver atrasado.
- Se o logger do Drive atrasar, ele marca slots perdidos como NULL (err=1) e nao replica posicoes; STOP continua no deadline alvo.
- a5_speed_logger cacheia modo de leitura de P0B-09 (FC03/FC04) para reduzir latencia de fallback em cada amostra.
- a5_speed_logger --ipc: aceita STOP via stdin para encerramento antecipado com a mesma rotina de parada.
- a5_speed_logger --ipc: aceita PAUSE/RESUME. Em pause aplica rampa ate 0 rpm e para; em resume volta com rampa de setpoint e desloca deadlines (sem contar tempo pausado).
- a5_speed_logger aplica rampa linear de setpoint (3 s) entre trocas de segmento, pause/resume e stop de ensaio para reduzir tranco no motor.
- a5_speed_logger escreve `a5_speed_events.log` ao lado do `drive.csv` com startup, START, pause/resume, progresso por segundo, erros de leitura e encerramento.
- merge_logs: junta dlg.csv + drive.csv por indice de linha e gera CSV de resultado com colunas: idx,t_s,ch1..ch8,atrito,pos,rpm,dlg_err,drive_pos_err,drive_rpm_err.
- Atrito por distancia (tempo real e final) e processado em Python no supervisorio com alinhamento por idx entre `dlg.csv` e `drive.csv`.
- Regra de distancia em Python usa unwrap orientado por direcao (RPM com deadband) e reconstrucao guiada por RPM/dt para tolerar gaps grandes entre amostras validas, mantendo guarda de plausibilidade.
- o arquivo _P por distancia e sempre reconstruido no final por `wait_and_merge` usando os CSVs completos.
- No supervisorio (agregacao em Python), `P0B-09` usa 1 ciclo por volta de motor e converte para distancia do pino por `dist_inc = (voltas_motor / i) * (2*pi*raio)` (i = D2/D1).
Field notes (DriveA5, based on recent tests):
- Relative mode (P11-04=0) is more consistent than absolute, but still drifts if completion threshold is loose.
- P05-21 (positioning completion threshold) around 20 caused ~20 count residual; lowering to 5 or 2 is recommended for tighter closure.
- P11-15=0 is unstable; use non-zero accel/decel (e.g., 50-150 ms) and small wait (P11-16=10-20 ms) to reduce settling error.
- P05-30=6 (home=current pos) frequently fails to write over Modbus; software zero offset is used when this happens.
- Modbus intermittency: many read/write failures show errno=17 ("File exists"). Expect retries; cached P0C-26 is required when reads fail.
- Verification tolerance in CLI is separate from drive completion; CLI tolerance should be <= P05-21.

Investigation notes - DriveA5 communication saga (2026-03-19)
- Contexto: em ensaios reais no laboratorio, o DLG permaneceu saudavel, mas a telemetria do Drive caiu fortemente. O controle de velocidade continuou funcionando, porem `P0B-09`/`P0B-00` passaram a falhar em alta taxa.
- Baseline historico bom: nos ensaios de 09-03-2026 o Drive chegou a ~80-88% de linhas com `pos` e `rpm` validos ao mesmo tempo.
- Cenario degradado observado em 19-03-2026: `drive_pos_err` ~82-87%, `drive_rpm_err` ~77-87% e forte subcontagem de voltas quando a velocidade de ensaio era maior.
- Ajuste 1 testado: aumentar timeouts rapidos + retry curto no logger do Drive. Resultado: pequena melhora, mas insuficiente; o link continuou muito abaixo do baseline historico.
- Ajuste 2 testado: leitura em bloco e reconstrucao de voltas com unwrap guiado por RPM/dt no Python. Resultado: melhorou a reconstrucao offline de voltas em gaps grandes, mas nao resolveu a causa-raiz da aquisicao ruim do Drive.
- Ajuste 3 testado: variar `P0C-25` no Drive (0, 2 e 4). Resultado: sem ganho relevante; `P0C-25=4` ficou ligeiramente pior. Nao tratar `P0C-25` como alavanca principal.
- Ajuste 4 testado: politica por deadline de slot, priorizando posicao e tentando RPM apenas no tempo restante. Resultado: nao melhorou a posicao de forma relevante e piorou muito a disponibilidade de RPM.
- Ajuste 5 testado: modo temporario somente posicao a 50 Hz. Resultado: ganho marginal em `pos_err`/`valid_pos`, insuficiente para justificar a perda total de RPM.
- Diagnostico importante obtido nos logs de slot: a folga do slot ficou baixa/quase nula; portanto o problema nao era "tempo sobrando mal aproveitado". O gargalo aparente esta na propria transacao Modbus/serial de `P0B-09` em ambiente real.
- Conclusao operacional desta saga: as mudancas exploratorias acima nao devem ser tratadas como novo baseline do projeto sem revalidacao forte. O ponto seguro continua sendo o ultimo commit estavel anterior a essa rodada de experimentos.
- Proxima frente recomendada fora do software: investigar adaptador RS485/Ethernet, cabo, EMC/roteamento no laboratorio, aterramento/shield e comportamento do conversor serial sob ruido do drive/carga.

Operational constraints
- Windows only (VS2022 + CMake). Use PowerShell at repo root.
- All executables must run on other Windows machines (self-contained distribution even if larger).
- DLG IP: 192.168.1.100, UDP port 41401. Open firewall IN/OUT.
- Do not change protocol structs or packing without validating offsets/endianness.
- If operating hardware: confirm IP/port and do not move actuators without explicit approval.

CSV conventions
- ASCII logs and CSV only.
- Fixed headers and units. Use "NULL" rows only for real losses.
- `resultado_ensaio.csv` and o arquivo _P por distancia use `;` as column delimiter.
- Arquivos finais do grafico 3: _DP.csv (distancia) e _VP.csv (volta), ambos com `;` como delimitador.
- o arquivo _P por distancia inclui `velocidade_media_mm_s` por intervalo, derivada de `rpm_medio_intervalo` com `v = |rpm| * (2*pi*raio) / (60*i)`.
- Write outputs into out/ subfolders (gitignored) when creating artifacts.
- Supervisor (novo_tribometro.py) grava em: Desktop\\Repositorio\\<AAAA-MM-DD - PoD - NomeEnsaio_Estudo-Repeticao>.
  Arquivos finais: <data>-<nome_ensaio>-<estudo>-<repeticao>_I.csv, _P.csv, _T.csv.
  Pasta tecnica: DadosDev\\ com <arquivo_t>.merge_source, dlg.csv, drive.csv, schedule.csv, graph_events.log, dlg_logger_events.log e a5_speed_events.log.

Roadmap (short)
- Load calibration (a,b) from file and apply on-the-fly in DLG logger.
- Formalize JSON protocol v1 (required/optional fields, errors).
- Unify both executables into one core with threads for motor/DLG/IPC.
- Parametrization via config file (duration, rate, channels, serial port, output dir).
- Add bench tests with simulated DLG and synthetic drive position.

Agent behavior
- Read the repo and produce a plan before editing, unless trivial.
- Propose focused diffs; avoid broad reformatting.
- Build the changed executable/app before final handoff when a local build path is available; for supervisor UI changes, regenerate `Supervisório\dist\novo_tribometro.exe` with the existing PyInstaller spec.
- If unexpected changes appear that you did not make, stop and ask.
- Keep AGENTS.md updated when protocol, calibration, or behavior changes are introduced.

Multi-exe orchestration
- Expect multiple executables (one per function). A supervisor program will orchestrate them.
- Executables should exchange data via files: e.g., calibration tool writes a file that the main DLG logger reads.

- Grafico 3 tem switch de visualizacao: Distancia (processamento por intervalo em mm) ou Volta (processamento por volta do pino).
- O ensaio gera dois arquivos finais do grafico 3: _DP.csv (distancia) e _VP.csv (volta).
