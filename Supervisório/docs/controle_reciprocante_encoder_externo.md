# Controle reciprocante pelo encoder externo

## Estado atual: controle autoritativo validado em bancada

O programa principal não requisita mais `P0B-09` (posição) nem `P0B-00`
(RPM) durante ensaios. No reciprocante, o CH3 do DLG é a única fonte de
posição, velocidade, inversão, extremos e distância concluída. O Drive é
somente atuador: recebe setpoint, RUN, inversão e STOP e mantém apenas o
readback de configuração/status de falha antes de `READY`.

Arquitetura:

1. `dlg_logger_ipc` adquire CH1..CH8 e calcula o estado do encoder a 200 Hz.
2. O DLG publica o estado causal por UDP em `127.0.0.1`, com sessão aleatória,
   sequência, QPC e CRC-32.
3. `a5_speed_logger --recip-encoder-control --command-only` é o único dono dos
   comandos Modbus e reage diretamente aos pacotes; Python e CSV não entram no
   caminho crítico.
4. O supervisório usa `dlg.csv + encoder_state.csv` para gerar `_T`, `_DP`,
   `_M` e os gráficos. `drive.csv` contém somente a trilha de comandos.

O modo histórico `--recip-encoder-shadow` permanece disponível apenas para o
programa `MapeiaParadaReciprocante`; ele não é usado pelo programa principal.

## Máquina de estados e segurança

- origem relativa na primeira amostra aceita, antes do movimento;
- sentido elétrico aprendido após 0,5 mm de deslocamento líquido na correção
  dinâmica e reutilizado no ensaio oficial;
- extremos fixos em `origem` e `origem +/- curso`;
- mediana causal de 15 amostras e confirmação física por 0,25 mm;
- gatilho único por stroke e troca de velocidade somente na inversão;
- comando de parada no gatilho, seguido de inversão após 20 ms;
- término somente em stroke completo;
- ausência de movimento/sentido por 3 s, pacote inválido prolongado, timeout do
  stroke, falha do encoder e deslocamento de 2x o curso causam STOP e falha;
- tolerância de extremo é diagnóstica e não invalida sozinha o ensaio;
- pausa é bloqueada no reciprocante autoritativo para preservar a referência
  causal dos extremos; o botão Parar continua disponível.

O modelo de antecipação aplicado em todas as velocidades é:

`antecipacao_mm = 0.13199835832212273 * abs(velocidade_mm_s) + 0.27025001630016243`

A velocidade é estimada por OLS causal nos 250 ms anteriores. A antecipação é
limitada a 45% do curso. O alvo físico não é deslocado. O modelo veio do mapa
de 66 paradas válidas (1..20 mm/s, cursos de 4..50 mm). A atuação real foi
validada no programa principal entre 1 e 3 mm/s; a ampliação progressiva até
20 mm/s continua sendo uma validação futura, não uma alteração de arquitetura.

## Processamento e arquivos

- `_T.csv`: 200 Hz; `pos` é posição relativa em mm, `rpm` é calculado no disco,
  e os antigos erros do Drive foram substituídos por `encoder_pos_err` e
  `encoder_rpm_err`.
- `_M.csv`: strokes delimitados pelos extremos físicos; velocidade média é
  curso físico/duração e velocidade alvo é a etapa travada naquele stroke.
- `_DP.csv`: distância acumulada pela soma dos cursos físicos e progresso
  projetado dentro do stroke, sem relação mecânica nem telemetria do Drive.
  No reciprocante, `atrito_med` é a média do módulo; `atrito_min` e
  `atrito_max` preservam o sinal. No contínuo, a média permanece assinada.
- `DadosDev/dlg.csv`: amostras brutas/calibradas do DLG a 200 Hz.
- `DadosDev/encoder_state.csv`: trilha causal completa do encoder.
- `DadosDev/drive.csv`: `idx,t_qpc,t_s,cmd_rpm,cmd_err`.
- `DadosDev/a5_speed_events.log`: gatilhos, comandos de inversão, extremos,
  latência, conclusão e falhas.

O último stroke não possui uma reversão posterior para confirmar o extremo.
Depois de `RECIP_ENCODER_DONE`, o supervisório usa o QPC desse evento para
iniciar a análise da cauda do CH3 e reconhece o primeiro extremo causal que
satisfaz todos estes critérios: mediana causal de 9 amostras, movimento mínimo
de 0,2 mm, pelo menos 250 ms de estabilidade, faixa de até 0,15 mm e velocidade
OLS em módulo de até 0,25 mm/s. O `_T` e o último stroke de `_M` são então
encerrados nesse extremo; a cauda técnica completa permanece em `DadosDev`.

O `_I.csv` tem sempre três colunas (`campo;valor;valor2`) e usa `;` como
separador. Valores que contenham `;` são protegidos pelas regras normais de CSV.
Arquivos históricos com separador `,` continuam aceitos pelo reprocessador
offline.

O erro de extremo registrado no `_I` é o erro do extremo físico confirmado,
não a distância entre gatilho antecipado e alvo. A correção dinâmica também
usa o encoder autoritativo, executa 1 ciclo de estabilização + 5 ciclos úteis a
1 RPM no disco e calcula o offset com média assinada equilibrada por sentido.

O controle autoritativo e a confirmação estacionária do último extremo foram
validados em bancada no ensaio `TesteEncoder_1-1` de 17/08/2026: 36 strokes,
curso médio de 9,809 mm, erro absoluto médio de 0,215 mm, extremo final a
-0,032 mm do alvo, 0,0223% de quarentena e `Pos-captura
reciprocante;encoder_estavel;` no `_I`.

## Evidência automatizada

- 38 testes Python do pipeline/processamento aprovados;
- 3 suites C do `EncoderCore` aprovadas;
- `a5_speed_logger --self-test` aprovado;
- `a5_speed_logger --self-test-encoder-link` aprovado;
- replay do ensaio `TesteEncoder_1-1` gerou 20 strokes, curso médio
  15,1916 mm e velocidade média 5,0255 mm/s sem consultar `drive.csv`.

## Validações futuras

O marco funcional em baixa velocidade está concluído. Para ampliar o domínio
operacional, executar ensaios graduais em 5, 10, 15 e 20 mm/s, sempre
confirmando no log e no `_I`:

- `DRIVE_PERIODIC_TELEMETRY_DISABLED`;
- `RECIP_SHADOW_READY ... action_enabled=1`;
- `RECIP_ENCODER_TRIGGER` e `RECIP_ENCODER_REVERSE_COMMAND` por stroke;
- `RECIP_ENCODER_DONE` no último stroke;
- `Pos-captura reciprocante;encoder_estavel;`;
- ausência de `RECIP_ENCODER_FAULT` e de falha de comando;
- cursos físicos, erro de extremo, perdas, quarentena e latência coerentes.

O modo contínuo não foi alterado por essa correção.
