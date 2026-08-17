# EncoderCore

Nucleo C causal para o encoder externo no CH3 do DLG. O programa principal usa
o estado diretamente nos modos continuo e reciprocante. No reciprocante, o
consumidor C do Drive tem autoridade para inverter e parar; Python/CSV ficam
fora do caminho critico.

## Contrato

- entrada temporal monotona por QPC;
- calibracao obrigatoria para entrada raw: schema 1, CH3, corrente, ganho x3,
  LPF0, unidade em graus e qualidade aprovada;
- zero relativo definido pela primeira amostra aceita;
- angulo normalizado, unwrap e distancia medidos diretamente no disco;
- velocidade do disco por regressao linear causal em janela temporal;
- amostras ausentes, saturadas ou em quarentena nunca movimentam o estado;
- innovation gate rejeita saltos fisicamente incompativeis sem mover a ancora;
- `STALE_STOP` e `FAILED` sao estados separados. Os valores padrao sao
  provisoriamente 100 ms e 3 s; o primeiro sera recalibrado por medicao de
  latencia/jitter antes da integracao com o motor;
- mudanca de sentido e extremo fisico sao confirmados causalmente;
- interpolacao de fronteira usa dois pontos aceitos e QPC.

O nucleo apenas informa estados. Parar, inverter ou habilitar o Drive pertence
ao `a5_speed_logger`, que recebe o enlace UDP local e escreve Modbus. O modo
sombra permanece somente para o mapeador de parada e diagnosticos isolados.

O pacote fica em `encoder_control_protocol.h`: 92 bytes, sessao por ensaio,
sequencia crescente, QPC, grandezas em milimetros, saude/status e CRC-32.

`recip_encoder_controller.h` contem a maquina reciprocante independente de
hardware. Ela usa extremos fixos no disco, mediana causal de 15 amostras
(75 ms de historico a 200 Hz), banda de chegada de 0.2 mm e histerese de
reversao de 0.25 mm. A reversao fisica e confirmada pelo deslocamento liquido
a partir do extremo observado; o flag de sentido instantaneo do EncoderCore
nao participa dessa decisao.

No reciprocante, caminho e termino sao calculados por strokes completos mais
o progresso projetado no stroke atual. A soma dos modulos entre amostras fica
apenas como diagnostico, pois ruido limitado pode inflar fortemente essa
grandeza. Extremos permanecem fixos em `origem` e `origem +/- curso`, sem
reancoragem cumulativa. A tolerancia em milimetros continua diagnostica e nao
reprova o ensaio.

Opcionalmente, o controlador reciprocante antecipa o gatilho com um modelo linear da
distancia de parada. A velocidade e estimada por OLS causal em janela temporal;
o alvo fisico permanece fixo e apenas a posicao do gatilho e deslocada. A
antecipacao tem limite configuravel como fracao do curso. Cada decisao expoe
alvo fisico, gatilho, antecipacao, velocidade estimada e flag de clamp para
auditoria. O recurso e desabilitado por padrao na biblioteca e habilitado
explicitamente pelo orquestrador no controle operacional.

O sentido eletrico/mecanico e aprendido na correcao dinamica pelo sinal do
deslocamento liquido sustentado de 0.5 mm, e entao reutilizado no ensaio
oficial. Isso evita escolher o sentido por uma unica amostra ruidosa.

`recip_encoder_replay` executa exatamente a maquina C sobre um
`encoder_state.csv`, aceita os parametros da compensacao e pode gravar um CSV
de eventos com `--events-out`, permitindo validar capturas reais sem hardware.

## Build

```powershell
cmake -S EncoderCore -B EncoderCore/build_vs2022 -G "Visual Studio 17 2022" -A x64
cmake --build EncoderCore/build_vs2022 --config Release
ctest --test-dir EncoderCore/build_vs2022 -C Release --output-on-failure
```

O `encoder_state_probe` existe apenas para paridade automatizada com os casos
compactos em `Supervisório/tests/fixtures/encoder_replay_cases.json`.
