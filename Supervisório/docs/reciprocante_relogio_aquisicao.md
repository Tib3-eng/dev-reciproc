# Relogio de aquisicao do reciprocante - 27/10/2026

O crescimento progressivo do arco foi associado ao crescimento da fila do
logger: uma amostra era consumida por slot nominal de 200 Hz e recebia o QPC
do slot. Uma diferenca pequena entre os relogios do DLG e do PC acumulava
idade no feedback. O diagnostico com os ensaios de setembro esta em
`out/recip_shift_20261027/RELATORIO.md` (artefato local, fora do Git).

O estado anterior foi preservado no commit `f47b189`.

## Caminho ativo

O supervisor habilita `dlg_logger_ipc --encoder-acquisition-clock` somente
quando `reciprocating_encoder_control_enabled=True`, inclusive na correcao
dinamica. O continuo, monitoramento, tara e sombra legada preservam os caminhos
anteriores. Nao houve mudanca em parametros do Drive ou calibracoes.

1. Uma thread recebe os datagramas DLG, valida comprimento, canais, taxa e
   quantidade declarada de bursts e processa os samples intercalados.
2. `dlg_sample_clock` usa frame, contagem e QPC de recepcao para estimar QPC
   causal por amostra. Duplicatas e pacotes fora de ordem nao renovam o
   encoder. A passagem uint32 do frame e aceita; um restart durante o ensaio
   nao redefine a origem mecanica.
3. O EncoderCore e a publicacao UDP executam nessa thread, antes da fila de
   arquivo. O QPC publicado e o estimado da amostra, e nao o horario da escrita.
4. Outra thread (o loop principal) grava os arquivos e atende STOP. A fila de
   gravacao tem limite de 8192 registros; seu crescimento nao envelhece o
   feedback. Esgotamento ou erro de arquivo e fatal e sinaliza FAILED ao Drive.

Os valores dos canais de um sample e seu estado causal permanecem unidos em
um registro. `dlg.csv` e `encoder_state.csv` mantem o mesmo idx, QPC e t_s.
O processamento Python verifica o alinhamento de indices e QPC no reciprocante.

## Semantica temporal e arquivos

- A taxa solicitada e nominalmente 200 Hz. Uma captura completa continua com
  `round(duracao * taxa)` linhas, incluindo NULLs; STOP/falha preserva o parcial.
- O espacamento por burst usa a frequencia informada pelo DLG (validada na
  faixa de 1% da solicitada), inclusive quando houver quantizacao dessa taxa.
  O valor aparece em `ACQUISITION_END reported_rate_hz`.
- `t_s = (source_qpc - origin_qpc) / QPC_frequency`. Nao pressupor que t_s seja
  exatamente idx/200 no novo fluxo. O intervalo real entre amostras pode variar
  ligeiramente e a duracao real de uma captura por contagem tambem.
- O QPC de recepcao e medido no PC. O timestamp do dispositivo e preservado
  como valor bruto; suas unidades nao sao assumidas para controlar o motor.
- O estimador corrige fase negativa contra a recepcao e limita a correcao
  positiva a 500 ppm. Assim acompanha a deriva observada sem converter uma
  rajada atrasada em dados recentes. O instante estimado nunca e posterior a
  recepcao; o atraso absoluto interno do hardware/rede ainda nao e medido.
- Lacunas de frame usam a contagem anterior por pacote para estimar samples
  ausentes. Em ausencia de pacotes ha graca de 50 ms para acomodar bursts,
  depois entram NULLs com horarios crescentes. A volta do pacote nao duplica
  NULLs nem reinsere dados anteriores ao ultimo horario ja materializado.
- `DadosDev/dlg_acquisition.csv` preserva frame, timestamp bruto, indice de
  burst, QPC de origem/recepcao/publicacao, idade, atraso da escrita e oito
  canais brutos. `err=0` indica sample recebido; `err=1`, NULL; `err=2`, bruto
  duplicado/fora de ordem/atrasado, sem linha operacional (`idx=-1`). Esses
  registros sem uso operacional nao publicam um novo estado aceito.
- Dados recebidos com idade >=100 ms permanecem no arquivo bruto, mas sao
  apresentados ao encoder operacional como ausentes.
- A correcao dinamica preserva a trilha como
  `DadosDev/recip_offset_dlg_acquisition.csv`.

## Protecoes

O supervisor combina o novo logger com
`a5_speed_logger --recip-source-max-age-ms 100`. A verificacao soma a idade
do QPC de origem e a idade da ultima medida aceita. Pacotes antigos/futuros
nao estabelecem origem nem renovam o controle. A ausencia de medida aceita
por mais de 100 ms dispara ENCODER_TIMEOUT; heartbeats ausentes nao renovam
esse prazo. O formato UDP v1 permanece com 92 bytes e CRC-32.

FAULT tem prioridade sobre COMPLETE/REVERSE; COMPLETE tem prioridade sobre
REVERSE. Uma decisao de inversao pendente e revista contra o estado terminal
do controlador antes de ser retirada. Isso evita que falha/termino durante
uma rajada sejam perdidos atras de uma inversao ja enfileirada.

O programa principal bloqueia pausa no reciprocante. Se PAUSE chegar
diretamente ao novo logger, ele equivale a STOP; nao cria uma nova origem nem
permite retomar com extremos diferentes. STOP antes da primeira amostra nao
publica uma origem ficticia. Se o logger morrer na validacao inicial, o
orquestrador nao envia START ao Drive.
O novo logger tambem encerra ao fechar o pipe do pai e consome comandos ja
bufferizados antes de aguardar novos bytes. START e STOP no mesmo envio nao
podem prender o STOP no buffer de entrada.

Extremos fixos, gate fisico RECIP_PHYSICAL_V1, filtro de 15, antecipacao,
limite de 2x curso, recuperacao Modbus e regras de termino foram preservados.

## Validacao e limite

O teste C do relogio cobre 37,5 min virtuais, +/-100 ppm, jitter, bursts,
frame wrap, duplicatas, reordenacao, perdas e atraso de recepcao. Um teste de
malha fechada usa o controlador C real com produtor de 200,003294 Hz e
mecanica ideal invariavel: os cursos fisicos dos trechos inicial/final devem
permanecer iguais, sem depender de ajuste de alvo por tempo.

O teste Windows de threads usa exclusivamente UDP 127.0.0.1: pausa artificial
da escrita nao pode interromper a publicacao; esgotar a fila deve sinalizar
FAILED. A integracao Python executa o logger de producao contra um DLG falso,
com calibracao isolada no ambiente do subprocesso, sem acessar o equipamento.

Comandos (raiz do projeto, apos compilar Release):

```powershell
DLG4000/build_vs2022/Release/dlg_sample_clock_tests.exe
DLG4000/build_vs2022/Release/dlg_encoder_stream_tests.exe out/fifo_fix_20261027/stream_tests
python DLG4000/tests/test_encoder_stream.py
DriveA5/build_vs2022/Release/a5_speed_logger.exe --self-test
DriveA5/build_vs2022/Release/a5_speed_logger.exe --self-test-encoder-link
```

Esses testes nao certificam a resposta mecanica real. A abertura progressiva
e o erro inicial de curso sao problemas separados: o modelo de antecipacao
precisa ser revalidado em bancada depois da correcao temporal, mantendo o
mesmo curso, carga e velocidade e observando os extremos durante um ensaio
longo. Nao foi aplicado ajuste empirico de frenagem nesta alteracao.
