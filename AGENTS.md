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
- dlg_logger_ipc: headless logger (8 canais) com CSV fixo (idx,t_qpc,t_s,ch1..ch8,err). Usa --ipc e espera "START" via stdin.
- dlg_logger_ipc grava numero fixo de linhas (duracao * taxa) e injeta NULL quando faltar amostra.
- dlg_logger_ipc aplica calibracao por canal quando calib.json/calib_CHn.json estiver presente; se nao houver, loga bruto.
- dlg_logger_ipc espera 3 amostras validas (DATA_OK) antes de iniciar o tempo; o supervisório so inicia o Drive depois disso.
- dlg_logger_ipc --ipc agora aceita PAUSE/RESUME/STOP. Em pause ele drena e descarta pacotes, congela o tempo local e retoma sem criar slots de catch-up.

CalibraDLG (UDP/WinSock2):
- Modes: interactive console (default) or --ipc (JSON lines over STDIN/STDOUT).
- IPC ops: config/point/finish/cancel; emits point_result/done/error.
- JSON output path can be set by IPC; defaults to out/calib.json.

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

Supervisório (Python/Tk):
- novo_tribometro.py is the main UI; novo_tribometro.exe is the packaged app.
- orchestrator_runtime.py launches dlg_logger_ipc + a5_speed_logger in background and merges logs with merge_logs.
- conversao mm/s->rpm no orchestrator usa modulo (abs) para enviar setpoint sempre positivo no modo velocidade.
- UI mostra "Velocidade alvo atual" (mm/s) e "RPM Alvo atual" (setpoint enviado ao Drive).
- A UI nao usa mais aba de log; mensagens seguem para console e status visuais.
- A aba "configuracoes adicionais" permite escolher diretorio base fixo dos ensaios; valor persistido em settings local (APPDATA).
- A aba "configuracoes adicionais" inclui botao "Configurar canais" para abrir CalibraDLG_UI diretamente, com bloqueio se ensaio/processos estiverem ativos.
- Botao "Configurar Eixos Y (Min / Max)" abre popup compacto com 2 linhas (Temperatura/Grafico 1 e Atrito-forca/Grafico 2), com modo Automatico (padrao) e campos Min/Max manuais.
- Taxa padrao de aquisicao no pipeline externo (DLG + Drive) ajustada para 50 Hz; manter ambas iguais para sincronismo.
- wait_and_merge has fallback merge in Python if merge_logs fails or merge.csv is missing.
- Outputs go to Desktop\\Repositorio\\<Nome do ensaio - Estudo> with info.csv, schedule.csv, dlg.csv, drive.csv, merge.csv.
- Check status: DLG uses UDP ACQSTOP/SETCH/SETUP/START (8 canais) and waits for ACQDATA (no ICMP ping).
- Check status bind: tenta 41402 (mesma do logger); se falhar, usa porta efemera e registra no log.
- Pending: If DLG check still fails, suspect DLG busy in another app or firewall/route issues.
- stop_run: envia STOP via stdin (IPC) e espera curto periodo antes de terminate/kill.
- stop_run no supervisório: ao clicar Parar, estado vira "Finalizando...", envia STOP para ambos, aguarda fechamento e mantem merge final ate salvar arquivos parciais.
- orchestrator_runtime tem pause_run/resume_run e envia PAUSE/RESUME para ambos os processos IPC.
- Botao Pausar no supervisório externo alterna para Retomar, congela o cronometro e o estado; ao retomar, continua da mesma etapa.
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
- a5_speed_logger: headless logger para modo velocidade (RPM) com schedule CSV (rpm,duration_s). Loga P0B-09 (posicao) e P0B-00 (actual motor speed) em 50 Hz: idx,t_qpc,t_s,pos,rpm,pos_err,rpm_err. Se P05-02 estiver disponivel, escala posicao para 0..(P05-02-1); caso contrario usa bruto 0..65535.
- a5_speed_logger --setup: escreve P02-00=0, P06-00=0, P06-01=3, P06-02=0, P03-02=0, P0C-09=1 e P31-00=0 para usar P06-03 como unica fonte de velocidade (evita offset por A+B).
- a5_speed_logger fim de ensaio: envia parada imediata reforcada (RPM=0 + CTRL RDY + P31-00=0 com retry curto).
- a5_speed_logger usa deadline real (QPC/wall-time) para disparar STOP no tempo alvo, mesmo se o loop de aquisicao estiver atrasado.
- Se o logger do Drive atrasar, ele marca slots perdidos como NULL (err=1) e nao replica posicoes; STOP continua no deadline alvo.
- a5_speed_logger cacheia modo de leitura de P0B-09 (FC03/FC04) para reduzir latencia de fallback em cada amostra.
- a5_speed_logger --ipc: aceita STOP via stdin para encerramento antecipado com a mesma rotina de parada.
- a5_speed_logger --ipc: aceita PAUSE/RESUME. Em pause aplica rampa ate 0 rpm e para; em resume volta com rampa de setpoint e desloca deadlines (sem contar tempo pausado).
- a5_speed_logger aplica rampa linear de setpoint (3 s) entre trocas de segmento, pause/resume e stop de ensaio para reduzir tranco no motor.
- merge_logs: junta dlg.csv + drive.csv por indice de linha e gera merge.csv com colunas: idx,t_s,ch1..ch8,pos,rpm,dlg_err,drive_pos_err,drive_rpm_err.
Field notes (DriveA5, based on recent tests):
- Relative mode (P11-04=0) is more consistent than absolute, but still drifts if completion threshold is loose.
- P05-21 (positioning completion threshold) around 20 caused ~20 count residual; lowering to 5 or 2 is recommended for tighter closure.
- P11-15=0 is unstable; use non-zero accel/decel (e.g., 50-150 ms) and small wait (P11-16=10-20 ms) to reduce settling error.
- P05-30=6 (home=current pos) frequently fails to write over Modbus; software zero offset is used when this happens.
- Modbus intermittency: many read/write failures show errno=17 ("File exists"). Expect retries; cached P0C-26 is required when reads fail.
- Verification tolerance in CLI is separate from drive completion; CLI tolerance should be <= P05-21.

Operational constraints
- Windows only (VS2022 + CMake). Use PowerShell at repo root.
- All executables must run on other Windows machines (self-contained distribution even if larger).
- DLG IP: 192.168.1.100, UDP port 41401. Open firewall IN/OUT.
- Do not change protocol structs or packing without validating offsets/endianness.
- If operating hardware: confirm IP/port and do not move actuators without explicit approval.

CSV conventions
- ASCII logs and CSV only.
- Fixed headers and units. Use "NULL" rows only for real losses.
- Write outputs into out/ subfolders (gitignored) when creating artifacts.
- Supervisor (novo_tribometro.py) grava em: Desktop\\Repositorio\\<Nome do ensaio - Estudo>.
  Arquivos padrao: info.csv, schedule.csv, dlg.csv, drive.csv, merge.csv.

Roadmap (short)
- Load calibration (a,b) from file and apply on-the-fly in DLG logger.
- Formalize JSON protocol v1 (required/optional fields, errors).
- Unify both executables into one core with threads for motor/DLG/IPC.
- Parametrization via config file (duration, rate, channels, serial port, output dir).
- Add bench tests with simulated DLG and synthetic drive position.

Agent behavior
- Read the repo and produce a plan before editing, unless trivial.
- Propose focused diffs; avoid broad reformatting.
- If unexpected changes appear that you did not make, stop and ask.
- Keep AGENTS.md updated when protocol, calibration, or behavior changes are introduced.

Multi-exe orchestration
- Expect multiple executables (one per function). A supervisor program will orchestrate them.
- Executables should exchange data via files: e.g., calibration tool writes a file that the main DLG logger reads.
