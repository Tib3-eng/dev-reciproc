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
- dlg_encoder_test: teste do encoder BRT25-A0M16bit-RT1-X3 no CH3. O preset usa tSensor=1, iGain=1 (x3), iLPF=0, iSensPwr=1 (2.5 V), balanco desligado, fInputDCImp=1 e fInputACImp=0; x10 foi abandonado porque a captura real saturou o A/D em raw=-32768. O handshake e faseado: STOP/drain -> SETCHCFG/espera -> GETCHCFG isolado -> SETUP/espera -> START -> 3 ACQDATA na taxa solicitada e com frames distintos/crescentes. Readback explicito divergente ou SET rejeitado bloqueiam; se GETCHCFG nao responder em duas tentativas, usa fluxo compativel SET/SETUP/START, ainda exige os 3 ACQDATA antes do motor e registra mode=FALLBACK. Apos DRIVE_STARTED, drena backlog DLG pre-movimento e reancora a captura. Calibracoes com qualquer preset diferente de x3/LPF0 sao rejeitadas.
- A autocalibracao motorizada do dlg_encoder_test usa 11 wraps para delimitar 10 revolucoes completas: 7 treinam o fit robusto raw->graus e 3 formam o holdout; depois de aprovar, refaz o modelo operacional com as 10. Alinha DLG 200 Hz e P0B-09 10 Hz por QPC; NULL/pos_err sao ignorados sem forward-fill, gaps de ate 0.5 s usam interpolacao temporal e saltos fisicamente impossiveis sao rejeitados. Respeita pos_mod, exclui 5 graus das bordas, agrupa em bins de 1 grau com pelo menos 8 amostras e exige 320 bins por volta. O JSON marca readback verdadeiro apenas no modo estrito; fallback fica identificado como FALLBACK_ACQDATA. O monitor de teste usa CH3 + mediana causal de 9; P0B-09 nao participa da posicao operacional.
- Limites provisorios do fit angular: RMSE <=0.5 grau, P95 <=1 grau, max <=2 graus, erro da relacao por volta <=1% e nenhuma saturacao em raw <=-32760 ou >=32760. Aplica os limites tanto aos bins do holdout quanto as amostras com a mediana causal de 9 usada pelo monitor. Reprova preserva o JSON anterior e mantem CSVs/logs.
- A autocalibracao motorizada carrega `relacao` de `%LOCALAPPDATA%\LATRIB\supervisorio_settings.json`, usa alvo de 1 RPM no encoder e envia `rpm_motor=round(i*rpm_encoder)` ao a5_speed_logger em COM5. Valor persistido observado: i=4. Fluxo: Drive READY parado -> preset/readback DLG -> Drive START -> 11 wraps -> Drive STOPPED -> ACQSTOP -> fit/holdout. Timeout motorizado 900 s.
- O JSON motorizado usa schema_version=1, purpose=encoder_ch3_angle_deg e unit=deg e e gravado atomicamente em `%LOCALAPPDATA%\LATRIB\calibrations\encoder_external_ch3.json`; nomes historicos ficam apenas como fallback de leitura para diagnostico. O monitor aplica raw->graus, normaliza em [0,360), usa mediana 9 e atualiza uma linha no maximo 2 vezes/s quando muda 0.01 grau. Zero eletrico nao define zero mecanico.
- dlg_encoder_test grava `<calib>_autocal_events.log`, CSV DLG com t_qpc e `<calib>_autocal_drive/drive.csv`. Registra DLG_HANDSHAKE por tentativa (envios, ACK, GET/match e valores reais, SETUP/START, taxa/frames ACQDATA, socket/rejeicao), DLG_RESYNC, ANGULAR_FIT, metricas de treino/holdout, relacao, saturacao e falhas. Replay: `--replay-autocal CSV --rate HZ --ratio I`; sem --ratio tenta carregar o supervisorio. Capturas antigas com 3 wraps nao podem aprovar o holdout novo.
- A opcao manual por wraps permanece apenas como diagnostico/normalizacao nominal 4-20 mA; a calibracao manual com referencia continua separada. Ligacao CH3 nos pinos DB9 8/1; a polaridade 8=I+ e 1=I- e inferencia a validar com calibrador limitado/Lynx. Use fonte externa 12-24 V; excitacao 2.5 V do canal nao alimenta o encoder.

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
- Campo "Corpo de prova (bloco)" aceita texto alfanumerico; manter apenas obrigatoriedade de preenchimento.
- conversao mm/s->rpm no orchestrator usa modulo (abs) para enviar setpoint sempre positivo no modo velocidade.
- UI mostra "Velocidade alvo atual" (mm/s) e "RPM Alvo atual" (setpoint enviado ao Drive).
- Tabela previa das etapas mostra "Velocidade real [mm/s]", calculada pela volta inversa do RPM inteiro enviado ao Drive: `v = |rpm| * (2*pi*raio) / (60*i)`.
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
- Pipeline principal: DLG/encoder a 200 Hz; Drive registra comandos a 50 Hz sem leituras periodicas P0B-09/P0B-00. Nao gerar dlg_compat_50hz no fluxo principal.
- wait_and_merge has fallback merge in Python if merge_logs fails or resultado_ensaio.csv is missing.
- wait_and_merge reconstroi os arquivos processados por distancia em Python ao final de todo ensaio para garantir consistencia com os CSVs completos.
- Rebuild offline em Python usa a mesma regra de wrap/backstep do C (sem `%` direto no delta) para evitar sobrecontagem de voltas por jitter.
- Outputs go to Desktop\\Repositorio\\<AAAA-MM-DD - PoD - NomeEnsaio_Estudo-Repeticao> with arquivos finais nomeados por padrao:
  <data>-<nome_ensaio>-<estudo>-<repeticao>_I.csv (info), _P.csv (atrito por distancia), _T.csv (resultado ensaio).
- info_ensaio.csv inclui bloco "Dados de calibracao" com fit do CH1 (slope, intercept, r2) usando a mesma busca do dlg_logger_ipc (calib.json/calib/calib_CH1.json/out + pasta do exe).
- Artefatos tecnicos finais ficam em Desktop\\Repositorio\\<...>\\DadosDev\\:
  <arquivo_t>.merge_source, dlg.csv, drive.csv, schedule.csv, graph_events.log, dlg_logger_events.log, a5_speed_events.log.
- Move de dlg.csv/drive.csv para DadosDev e feito em duas passadas (wait_and_merge + finalize_pos-run) para evitar copia sem recorte quando houver lock temporario no Windows.
- Duplicidade de pasta e bloqueio de inicio consideram data + estudo + nome + repeticao na pasta unica da execucao.
- Se a pasta da execucao ja existir, o supervisÃ³rio pergunta se deve sobrescrever; ao confirmar duas vezes, apaga a pasta existente e reutiliza a mesma repeticao.
- Sobrescrita de repeticao usa remocao com retries/read-only; se houver arquivo aberto por Excel/visualizador, cancela com erro acionavel para evitar misturar arquivos antigos e novos.
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
- Modo reciprocante executa tara estatica obrigatoria e depois correcao dinamica por ensaio: quantidade persistente (padrao 5 ciclos validos), 1 ciclo extra de estabilizacao descartado, 1 ciclo = ida+volta, disco alvo em 1 RPM (`rpm_drive=round(i)`) e mesmo curso configurado. O offset da fase usa peso igual para as medias de ida e volta de cada ciclo valido.
- A correcao dinamica usa a media aritmetica assinada de CH1 como offset aditivo em N; nao altera calib_CH1.json. Reprova antes do ensaio oficial se os strokes nao completarem, algum logger falhar ou a perda DLG exceder 5%.
- No reciprocante, o offset e aplicado apenas aos dados processados/exibidos (CH1/atrito em _T, _DP, _M e graficos); DadosDev/dlg.csv permanece bruto. _M usa media do modulo, RMS permanece em ATRITO_EFETIVO e max/min preservam sinal.
- O arquivo _T grava `PosEncExt` filtrada e, imediatamente ao lado, `PosEncExt_Quarentena` (0=amostra aceita, 1=trecho reconstruido). O filtro operacional usa rastreamento angular por predicao, innovation gate e histerese de 5 amostras; somente transicoes fisicamente incompativeis sao isoladas e reconstruidas entre ancoras confiaveis. O CH3 bruto permanece em DadosDev/dlg.csv.
- No reciprocante, cada velocidade alvo fica travada durante todo o stroke. Mudancas de etapa que vencerem durante o movimento ficam pendentes e so sao aplicadas na inversao seguinte; `_M` registra `VELOCIDADE_MEDIA` e a `VELOCIDADE_ALVO` digitada para cada stroke.
- No programa principal, o reciprocante usa o encoder externo como autoridade unica para inversao, termino, posicao, velocidade e distancia. `a5_speed_logger --recip-encoder-control --command-only` recebe pacotes UDP locais a 200 Hz e o Drive atua apenas por comandos Modbus.
- O controle reciprocante externo exige amostra valida antes do movimento, aprende/valida sentido em 0.5 mm, aborta sem movimento em 3 s, usa extremos fixos, para no gatilho, espera 20 ms e inverte. Pausa e bloqueada neste modo para preservar extremos; STOP permanece disponivel.
- Barreira de partida reciprocante: `DATA_OK` do DLG pode anteceder o primeiro pacote UDP causal. Depois de START, `a5_speed_logger` mantem o Drive parado e espera ate 3 s por pacote CH3 aceito/inicializado/saudavel; somente entao inicia o cronometro e envia RUN. STOP/PAUSE cancelam a espera. Timeout registra `FATAL RECIP_ENCODER_NO_ORIGIN` e o supervisor mostra essa causa primaria sem classificar a captura inexistente como perda/ciclos invalidos.
- Modelo operacional de antecipacao: `0.13199835832212273 * abs(v_mm_s) + 0.27025001630016243`, OLS causal 250 ms e clamp em 45% do curso. O erro de extremo e medido no extremo fisico confirmado, nao no gatilho antecipado.
- `_T`, `_DP`, `_M` e graficos reciprocantes sao gerados de `dlg.csv + encoder_state.csv` a 200 Hz, sem abrir telemetria do Drive. `drive.csv` principal tem `idx,t_qpc,t_s,cmd_rpm,cmd_err`.
- Ao concluir a distancia reciprocante, o motor para imediatamente, mas Drive e DLG permanecem em pos-captura por ate 2 s. O orquestrador encerra quando CH3 apresenta o ultimo extremo e estabiliza; DadosDev preserva a cauda e `_T` e recortado no extremo externo detectado.
- O ultimo extremo reciprocante sem reversao e confirmado na cauda do CH3 a partir do QPC de RECIP_ENCODER_DONE: mediana causal 9, movimento >=0.2 mm, janela estavel 250 ms, faixa <=0.15 mm e |OLS| <=0.25 mm/s. `_T`/`_M` terminam no primeiro extremo causal aprovado; DadosDev preserva a cauda.
- Graficos reciprocantes usam processamento por stroke no lugar de volta. A velocidade media e curso efetivo dividido pela duracao do stroke, sem media assinada de RPM nem reconversao de mm/s; o PNG final recarrega `_M`/`_DP` reconstruidos.
- Na correcao dinamica, o orquestrador encerra os loggers ao completar os strokes do ciclo extra + ciclos validos, sem aguardar o watchdog. DadosDev/recip_dynamic_offset.csv marca as amostras usadas; _I registra offset, ciclos, RPM do disco/Drive, amostras validas, perda e validacao.
- Inicio de novo ensaio fica bloqueado enquanto tara, processos externos, merge/finalizacao ou salvamento dos arquivos finais do ensaio anterior ainda estiverem em andamento.
- Log da tara automatica fica em `<pasta do ensaio>\\DadosDev\\zero_ensaio.csv`; tara manual (botao "Zerar celula") grava em `<REPO_BASE>\\ZeroAvulso\\zero_avulso_<timestamp>.csv`.
- Busca do arquivo de calibracao CH1 prioriza `calib_CH1.json`/`out\\calib_CH1.json`; arquivos genericos (`calib.json`/`calib`) so sao aceitos quando o payload indicar CH1.
- novo_tribometro captures run state snapshot to avoid race with global external_run_state.
- Todo ensaio exige a calibracao angular aprovada do encoder em `%LOCALAPPDATA%\LATRIB\calibrations\encoder_external_ch3.json`; ausencia, schema/preset incompativel, saturacao ou quality.accepted=false bloqueiam antes da tara e de qualquer movimento.
- O dlg_logger_ipc aplica ao CH3 o preset e o fit do JSON angular e normaliza em [0,360), mas preserva a aquisicao sem filtro temporal em DadosDev/dlg.csv. No `_T.csv`, o supervisor grava `PosEncExt` e `PosEncExt_Quarentena`; `_DP` e `_M` usam somente amostras aceitas do encoder. O `_I.csv` registra caminho, SHA-256, schema, data, fit, zero, preset, taxa, referencia, relacao, split 7/3, metricas, perdas, saturacoes e estatisticas de quarentena; nao registra modelo do encoder.
- Tarefa 5A concluida: `encoder_control_protocol` define pacote binario v1 de 92 bytes com sessao, sequencia, QPC, estado causal em mm, saude/status e CRC-32. `dlg_logger_ipc` publica por UDP somente em 127.0.0.1 quando recebe porta+sessao explicitas. `a5_speed_logger --self-test-encoder-link` valida loopback/sessao/sequencia/stale sem hardware; esse caminho e autoritativo nas inversoes reais do programa principal.
- `recip_encoder_controller` e autoritativo no programa principal: extremos fixos, uma inversao por stroke, confirmacao da reversao fisica, conclusao somente no extremo, tolerancia diagnostica, timeout de encoder/curso e falha em 2x o curso. O modulo decide e `a5_speed_logger` executa Modbus; P0B-09 nao participa.
- Modo continuo migrado para CH3 externo: DLG/EncoderCore a 200 Hz gera encoder_state.csv, _T, _DP e _VP sem usar posicao/RPM do Drive. A direcao usa mediana causal de 51 amostras e deslocamento liquido desde a origem; trava depois de 5 confirmacoes acima de 1 mm, falha se nao travar em 3 s apos detectar 0.2 mm de movimento ou se ficar 1 mm incompativel com o sentido. O progresso e monotono contra jitter, interpola o alvo e usa gate fisico com 2x da maior velocidade entre alvo digitado e RPM inteiro efetivo. Essa logica e exclusiva do continuo; nao alterar o reciprocante ao corrigi-la.

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
- a5_speed_logger: headless logger para modo velocidade (RPM) com schedule CSV (rpm,duration_s). O modo legado/mapeador loga P0B-09 e P0B-00; o programa principal usa `--command-only` e nao faz essas leituras periodicas.
- a5_speed_logger --setup: escreve P02-00=0, P06-00=0, P06-01=3, P06-02=0, P03-02=0, P0C-09=1 e P31-00=0 para usar P06-03 como unica fonte de velocidade (evita offset por A+B).
- a5_speed_logger fim de ensaio: envia parada imediata reforcada (RPM=0 + CTRL RDY + P31-00=0 com retry curto).
- a5_speed_logger usa deadline real (QPC/wall-time) para disparar STOP no tempo alvo, mesmo se o loop de aquisicao estiver atrasado.
- Se o logger do Drive atrasar, ele marca slots perdidos como NULL (err=1) e nao replica posicoes; STOP continua no deadline alvo.
- a5_speed_logger cacheia modo de leitura de P0B-09 (FC03/FC04) para reduzir latencia de fallback em cada amostra.
- a5_speed_logger --ipc: aceita STOP via stdin para encerramento antecipado com a mesma rotina de parada.
- a5_speed_logger --ipc: aceita PAUSE/RESUME. Em pause aplica rampa ate 0 rpm e para; em resume volta com rampa de setpoint e desloca deadlines (sem contar tempo pausado).
- a5_speed_logger `--encoder-calibration` exige `--ipc` e `--setup`, para antes/depois do setup e exige escrita + readback dos 7 parametros do modo velocidade antes de READY. Cada WRITE/READBACK do setup e preservado em a5_speed_events.log, mesmo quando o pai suprime as confirmacoes OK no console. Depois exige 3 leituras P0B-09 validas e le somente posicao durante a captura. STARTED e emitido depois de aceitar zero+RUN e iniciar rampa de 3 s; perda de P0B-09 por 2 s aborta. Emite `STATUS_DRIVE` 1 vez/s com comunicacao, comando, P0B-09, contagem desenrolada/voltas, erros e idade da leitura. PAUSE equivale a STOP nesse modo. STOP/EOF/Ctrl+C convergem para rampa e parada reforcada; STOPPED so e emitido se RPM=0, CTRL RDY e P31-00 tiverem ao menos um comando aceito. O parser IPC consome comandos ja bufferizados antes de consultar novos bytes.
- a5_speed_logger `--self-test` valida a fila IPC sem abrir COM. O CMake copia modbus-5.dll por configuracao; Release deve receber a DLL de `x64-windows/bin`, nunca a de `debug/bin`.
- a5_speed_logger aplica rampa linear de setpoint (3 s) entre trocas de segmento, pause/resume e stop de ensaio para reduzir tranco no motor.
- a5_speed_logger tem modo reciprocante experimental em velocidade: usa RPM com sinal, P0B-09 desenrolado por direcao, curso em mm convertido por raio+relacao+P05-02/65536, e inverte em faixa `target +/- tolerancia_counts`.
- No modo reciprocante, o curso e uma ida ou volta e os extremos nominais ficam fixos. A parada e disparada ao atingir/cruzar o extremo; sair da tolerancia vira diagnostico e nao aborta. Um stroke que alcance 2x o curso configurado, perda prolongada de posicao ou fim de tempo sem distancia alvo retorna falha e preserva os dados parciais.
- No reciprocante, distancia e o criterio de termino e o tempo teorico recebe margem `max(1.5x, +30 s)` apenas como watchdog. RECIP_DONE ocorre depois de fechar um stroke; ao terminar Drive ou DLG, o orquestrador para o processo par para evitar cauda estatica ou movimento sem aquisicao.
- Reciprocante inicial usa paradas secas sem rampa. Tolerancia de parada fica persistida no supervisorio em "configuracoes adicionais".
- Alternativa registrada caso a conversao geometrica do curso nao funcione em bancada: calibrar empiricamente com volta lenta, lendo arquivo com tempo/RPM/posicao e usando a posicao real de parada como alvo final.
- a5_speed_logger escreve `a5_speed_events.log` ao lado do `drive.csv` com startup, START, pause/resume, progresso por segundo, erros de leitura e encerramento.
- Modo sombra reciprocante historico: permanece somente no MapeiaParadaReciprocante e em diagnosticos que pedirem `action_enabled=0`. No programa principal, o mesmo enlace UDP/EncoderCore atua com `action_enabled=1`; P0B-09 nao controla o movimento.
- O sentido do encoder reciprocante e aprendido na correcao dinamica por deslocamento liquido de 0.5 mm e reutilizado no ensaio oficial. A maquina sombra usa mediana causal 15, banda de extremo 0.2 mm, histerese de reversao 0.25 mm, extremos fixos e percurso por strokes concluidos. Nunca usar `path_distance_mm` bruto para terminar o reciprocante: ruido limitado inflou 480 mm para 5403.8 mm no teste de 13-08-2026.
- Antecipacao de parada reciprocante esta ativa no programa principal (`action_enabled=1`): `trigger = physical_target - direction * min(0.13199835832212273*abs(v_OLS_250ms)+0.27025001630016243, 0.45*course)`. Logs registram alvo fisico, gatilho, velocidade, antecipacao e clamp; `_I` registra o modelo. Atuacao real validada de 1 a 3 mm/s; validar gradualmente 5, 10, 15 e 20 mm/s.
- merge_logs e formato com drive_pos_err/drive_rpm_err ficam apenas para compatibilidade legada. O programa principal gera `_T` diretamente do DLG/encoder com encoder_pos_err/encoder_rpm_err.
- No programa principal, atrito por distancia (tempo real e final) usa `dlg.csv + encoder_state.csv`, alinhados por indice do mesmo pipeline DLG; `drive.csv` nao participa.
- No reciprocante, `_DP.atrito_med` e a media do modulo para impedir cancelamento entre sentidos; `_DP.atrito_min` e `_DP.atrito_max` preservam o sinal. No continuo, `atrito_med` permanece assinado.
- Regras baseadas em P0B-09/RPM e alinhamento `dlg.csv + drive.csv` permanecem somente nos caminhos legados e nas ferramentas de diagnostico/mapeamento.

Stopping response mapper (MapeiaParadaReciprocante):
- `mapa_parada.py` is a separate friendly-menu program; packaged executable is `MapeiaParadaReciprocante/dist/mapa_parada_reciprocante.exe` and embeds dlg_logger_ipc, a5_speed_logger, merge_logs and modbus-5.dll.
- Hardware sequence is gradual: speeds [1,2,5,10,15,20] mm/s and courses [50,30,15,4] mm. The C Drive logger remains the actuation/deadline owner; CH3 external encoder is diagnostic/measurement.
- Mapping calls a5_speed_logger with `--strict-setup`: reinforced STOP before/after setup, readback of all seven speed parameters, three consecutive valid P0B-09 reads and P0B-34=0 are required before READY.
- DLG must be valid before Drive START. Gates stop the whole matrix for DLG loss >1%, encoder shadow fault, missing strict preflight, sustained latency (mean >20 ms or >2% packets above 100 ms), negative incompatible stopping distance, stopping distance >= course or physical course >=2x configured.
- Statistical insufficiency is non-fatal: it invalidates that condition and blocks shorter courses at the same speed, then allows the next speed to restart at 50 mm. Encoder latency between 0.5% and 2% above 100 ms is a recorded warning.
- Each condition currently uses 6 strokes and discards the first complete cycle. Because the last physical extreme is not confirmed until reversal, accepted conditions leave 3 raw useful stops (2 in one direction, 1 in the other); modeling balances this as one mean per condition+direction.
- Completed study 2026-08-13: `out/stopping_curve/pilot_v1_c50`; 24 conditions executed safely, 22 accepted, 66 raw stops and 44 direction-balanced model points. Conditions 15 mm/s x 4 mm and 20 mm/s x 4 mm were excluded only for fewer than 3 complete stops after warmup.
- Observed range: external speed 0.994..20.806 mm/s and stopping distance 0.030..2.800 mm. Selected leave-one-condition-out model: `stopping_mm = 0.1319983583 * abs(external_speed_mm_s)`; CV RMSE 0.1296 mm. Balanced-point P95 CV margin is +0.21995 mm. For future per-stroke actuation use the raw-stop one-sided P95 +0.27025 mm (raw maximum +0.33931 mm), subject to independent validation.
- Course response surface did not improve condition-held-out validation once actual external speed was used. Course still affects whether short strokes reach target speed; do not extrapolate the rejected 15/4 and 20/4 conditions.
- Mapping artifacts: `pontos_parada_consolidados.csv` (raw), `pontos_modelo_balanceados.csv`, `modelo_parada.json`, and `RELATORIO_MAPA_PARADA.md`. The selected model is enabled in the main reciprocating controller, validated at 1..3 mm/s, and still requires gradual validation at 5/10/15/20 mm/s.
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
- Conclusao operacional desta saga: a telemetria periodica P0B-09/P0B-00 foi retirada do pipeline principal. O baseline atual e o pipeline CH3/EncoderCore a 200 Hz validado em bancada; as tentativas Modbus acima sao apenas historico e nao devem ser reintroduzidas.
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
- Arquivos finais `_T`, `_DP`, `_VP`, `_M` e `_I` usam `;` como delimitador.
- `_I.csv` usa sempre tres colunas `campo;valor;valor2` separadas por `;`. ReprocessaEncoder detecta automaticamente esse formato e o legado separado por `,`.
- Arquivos finais do grafico 3: _DP.csv (distancia) e _VP.csv (volta), ambos com `;` como delimitador.
- No modo reciprocante, _VP.csv e substituido por _M.csv, processado por stroke. _M.csv usa `TEMPO_(min);STROKE;ATRITO_EFETIVO;ATRITO_MEDIO;ATRITO_MAX;POS_MAX;ATRITO_MIN;POS_MIN;LINHAS;VELOCIDADE_MEDIA;VELOCIDADE_ALVO;ERRO_CURSO_MM`.
- No reciprocante, `_DP.atrito_med` e `_M.ATRITO_MEDIO` usam media do modulo; min/max preservam sinal. No continuo, a media de `_DP`/`_VP` permanece assinada.
- No _M.csv, o filtro de borda e configuravel no supervisorio em percentual por borda: 1% remove 1% do inicio e 1% do fim de cada stroke. O filtro se aplica a RMS, media, maximo e minimo; POS_MAX/POS_MIN sao em mm dentro do stroke.
- _DP.csv e _VP.csv incluem `t_s_inicio`: tempo da primeira amostra DLG/encoder que entrou naquele intervalo/volta; _M.csv usa `TEMPO_(min)` no inicio do stroke.
- Ao finalizar o ensaio, o supervisor salva _G.png com a figura 2x2 da aba "graficos".
- `_DP.velocidade_media_mm_s` vem da distancia exata dividida pelo intervalo entre cruzamentos do encoder externo; nao reconverter RPM do Drive no pipeline principal.
- Write outputs into out/ subfolders (gitignored) when creating artifacts.
- Supervisor (novo_tribometro.py) grava em: Desktop\\Repositorio\\<AAAA-MM-DD - PoD - NomeEnsaio_Estudo-Repeticao>.
  Arquivos finais: <data>-<nome_ensaio>-<estudo>-<repeticao>_I.csv, _DP.csv, _VP.csv (continuo) ou _M.csv (reciprocante), _T.csv, _G.png.
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
- Before any rollback that discards current changes, ask whether to preserve the current state in a backup branch/commit. Do not discard it until the user answers, unless the user explicitly requested immediate discard without backup.
- Build the changed executable/app before final handoff when a local build path is available; for supervisor UI changes, always regenerate and overwrite `C:\Users\nicol\Desktop\DevReciproc\Supervisório\dist\novo_tribometro.exe` with the existing PyInstaller spec. Do not create suffixed variants such as `_novo` or `_atualizado`.
- For DLG4000 production builds, always overwrite the executables in `C:\Users\nicol\Desktop\DevReciproc\DLG4000\bin\Release`. Do not keep or publish alternate production copies in folders such as `ReleaseAtualizado`; remove stale copies that could be launched by mistake.
- If unexpected changes appear that you did not make, stop and ask.
- Keep AGENTS.md updated when protocol, calibration, or behavior changes are introduced.

Multi-exe orchestration
- Expect multiple executables (one per function). A supervisor program will orchestrate them.
- Executables should exchange data via files: e.g., calibration tool writes a file that the main DLG logger reads.

- Grafico 3 tem switch de visualizacao: Distancia (processamento por intervalo em mm) ou Volta (processamento por volta do pino).
- O ensaio gera dois arquivos finais do grafico 3: _DP.csv (distancia) e _VP.csv (volta) no continuo; no reciprocante gera _DP.csv e _M.csv (stroke).
