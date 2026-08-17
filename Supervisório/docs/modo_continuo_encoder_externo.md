# Modo continuo pelo encoder externo

## Contrato operacional

No modo continuo, o CH3 do DLG a 200 Hz e a fonte de:

- posicao relativa e progresso do ensaio;
- distancia e voltas;
- velocidade media por intervalo/volta;
- RPM calculado do disco;
- termino normal pela distancia programada.

O Drive permanece responsavel pelo comando do motor. `drive.csv` e preservado
em `DadosDev` apenas como diagnostico nesta etapa e nao participa de `_T`,
`_DP`, `_VP`, graficos de distancia ou decisao de termino.

## Progresso e parada

A direcao continua usa a mediana causal das ultimas 51 amostras aceitas
(aproximadamente 255 ms a 200 Hz) e o deslocamento liquido desde a origem. A
trava ocorre depois de cinco confirmacoes consecutivas quando esse deslocamento
filtrado supera 1 mm. Assim, a oscilacao pontual do sinal durante a transicao
nao pode definir o sentido pelo sinal de incrementos isolados.

O progresso e a posicao relativa projetada na direcao travada, com protecao
monotona: pequenos retornos por jitter nao retiram distancia ja percorrida nem
sao somados como distancia nova. Se o sentido nao for definido em ate 3 s
depois que o deslocamento filtrado indicar movimento (0,2 mm), o ensaio para
com `DIRECTION_LOCK_TIMEOUT`. Se a posicao filtrada ultrapassar 1 mm no sentido
oposto ao ja travado, para com `DIRECTION_INCONSISTENT`.

O nucleo C do encoder aplica gate de inovacao e nunca cria movimento a partir
de uma amostra rejeitada. Tres segundos consecutivos sem amostra aceita geram
`ENCODER_FAILED` e parada dos dois processos. Ao cruzar a distancia alvo, o
DLG interpola o instante entre as duas amostras e emite
`ENCODER_TARGET_REACHED`; o orquestrador retransmite `STOP` ao DLG e ao Drive.

O limite fisico entregue ao nucleo considera o RPM inteiro efetivamente enviado
ao Drive, convertido para velocidade no disco, e aplica margem de 2x para os
transitorios. Portanto ele nao depende somente da velocidade decimal digitada.

O tempo configurado nao define mais sucesso. Ele e um deadline independente:
`max(1,5 * tempo_teorico, tempo_teorico + 30 s)`. Encerrar pelo deadline sem
atingir a distancia externa reprova o ensaio, preservando os dados parciais.

## Arquivos

- `_T.csv`: 200 Hz. `pos` e posicao relativa do encoder em mm; `rpm` e RPM do
  disco calculado do encoder. Os campos de erro sao `encoder_pos_err` e
  `encoder_rpm_err`.
- `_DP.csv`: intervalos de distancia calculados por cruzamento interpolado.
- `_VP.csv`: voltas do disco calculadas por cruzamento interpolado.
- `DadosDev/encoder_state.csv`: estado causal completo produzido pelo nucleo C.
- `DadosDev/dlg.csv`: aquisicao DLG completa a 200 Hz.
- `DadosDev/drive.csv`: somente diagnostico temporario.

A velocidade de cada bloco e `distancia_exata / intervalo_entre_cruzamentos`.
Isso evita o erro de tratar N amostras como N intervalos.

## Limites desta etapa

- A trava de sentido e o progresso monotono deste documento pertencem somente
  ao continuo. O controlador, extremos, filtros e processamento reciprocantes
  nao usam esse caminho e nao foram alterados por esta correcao.
- As mudancas de setpoint continuo ainda seguem o cronograma temporal enviado
  ao Drive. A medicao e o termino, contudo, usam somente o encoder externo.
- As leituras periodicas de posicao/RPM do Drive ja foram removidas do programa
  principal. Caminhos legados continuam apenas nas ferramentas de diagnostico
  e mapeamento que os solicitam explicitamente.

## Validacao de bancada

Confirmar no `_I.csv` `Status termino encoder continuo=alvo_atingido`. Conferir
tambem no `dlg_logger_events.log` o evento `ENCODER_TARGET_REACHED`, taxa
efetiva proxima de 200 Hz, `ring_overrun=0` e perda aceitavel. O `_DP` deve
mostrar a velocidade de cada intervalo proxima do alvo, incluindo os instantes
de troca de velocidade.

O teste de regressao inclui replay do ensaio real `TesteEncoder_3-1` de
17/08/2026: a direcao correta deve ser positiva, a trava deve ocorrer entre
2,5 s e 2,7 s e o cruzamento de 660 mm deve ocorrer por volta de 240,36 s.
