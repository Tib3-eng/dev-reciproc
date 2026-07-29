# Teste do encoder 4-20 mA no CH3

## Encoder registrado

Modelo: `BRT25-A0M16bit-RT1-X3`

Decodificacao confirmada pelo manual:

- `BRT25`: corpo de 25 mm e eixo solido de 4 mm;
- `A0M`: saida analogica de 4-20 mA;
- `16bit`: resolucao nominal de 65.536 divisoes por volta;
- `RT1`: cabo com saida lateral;
- `X3`: codigo interno de identificacao.

`16bit` nao significa 16 voltas. O codigo informado nao identifica sozinho a
variante de tres ou quatro fios. Confira a etiqueta e o cabo da unidade antes
da ligacao.

Documentos do projeto:

- [Manual do encoder](../.codex/docs/Manual%20Encoder.pdf)
- [Manual do DLG4000](../.codex/docs/Manual%20-%20DLG4000.pdf)

## Executavel e menu

Abra diretamente, sem comandos auxiliares:

```text
DLG4000\bin\Release\dlg_encoder_test.exe
```

Identificador esperado desta versao:

```text
2026-07-29-gain3-drivefilter-r4
```

Menu:

```text
1 - Monitorar CH3 (angulo e sinal 4-20 mA)
2 - Autocalibrar graus com Drive (4 wraps / 3 voltas)
3 - Diagnostico nominal manual (4 transicoes)
4 - Calibracao de corrente com referencia
5 - Verificar comunicacao com o DLG
6 - Configuracoes
7 - Modelo e esquema de ligacao
8 - Executar autoteste interno
0 - Sair
```

Cada operacao abre e fecha sua propria conexao. Ao terminar ou cancelar, o
programa envia `ACQSTOP`. Durante leituras, `Q` ou `Ctrl+C` cancela.

## Preset do CH3

O programa reproduz a configuracao que ficou estavel no software da Lynx:

| Campo | Valor |
|---|---:|
| Canal | CH3 |
| Entrada | Corrente |
| `tSensor` | 1 |
| Ganho | x3 |
| `iGain` | 1 |
| Excitacao | 2,5 V |
| `iSensPwr` | 1 |
| LPF adicional do programa | nenhum |
| `iLPF` | 0 |
| Balanco | desativado / zero |
| Impedancia de entrada DC | conectada, 100 kohm |
| `fInputDCImp` | 1 |
| Impedancia de entrada AC | desconectada |
| `fInputACImp` | 0 |

O handshake e feito em fases:

1. `ACQSTOP` e drenagem do socket;
2. `SETCHCFG` e uma janela exclusiva para o ACK;
3. ate duas solicitacoes `GETCHCFG`, comparando o CH3 campo a campo;
4. `ACQSETUP`, uma curta janela de resposta, e somente depois `ACQSTART`;
5. tres pacotes `ACQDATA` na taxa solicitada e com frames distintos/crescentes.

O readback exato confirma o preset mesmo se o ACK do `SETCHCFG` tiver se
perdido. Uma divergencia explicita ou rejeicao do `SETCHCFG` bloqueia a
aquisicao e nunca e ignorada.

Algumas revisoes de firmware nao respondem ao `GETCHCFG`. Depois de duas
tentativas estritas, o programa ativa o fluxo compativel ja usado pelos
loggers funcionais: `SETCHCFG -> ACQSETUP -> ACQSTART -> ACQDATA`. Nesse caso,
ele ainda exige tres pacotes de dados antes de autorizar o motor e registra
`mode=FALLBACK`; o preset foi enviado, mas nao houve confirmacao independente
por readback. Na autocalibracao motorizada e na opcao de verificacao, essa
condicao tambem e mostrada ao operador.

Nao foi adicionado LPF no processamento do programa. O protocolo do DLG,
entretanto, exige um indice `iLPF` e o manual nao documenta um valor separado
para "desligado"; por isso o preset permanece em `iLPF=0`, como na convencao
atual do teste.

O restante do preset reproduz a configuracao estavel observada na Lynx, mas o
ganho foi reduzido de x10 para x3. A ultima captura em x10 atingiu o limite
inferior do A/D (`raw=-32768`) em 427 amostras; portanto aquela escala nao
preservava toda a informacao do encoder. Qualquer amostra em
`raw <= -32760` ou `raw >= 32760` continua reprovando a autocalibracao e
fica registrada no log.

## Autocalibracao angular com Drive

A opcao 2 substitui a antiga metodologia baseada apenas em dois extremos. Ela
correlaciona todo o percurso do encoder com `P0B-09` e produz diretamente:

```text
graus = slope * raw + intercept
```

A saida do monitor e normalizada em `[0, 360)`.

### Movimento e sincronismo

O alvo padrao e 1 RPM no eixo do encoder. O comando do motor e:

```text
rpm_motor = arredondar(rpm_encoder * i)
i = D2 / D1
```

A relacao e carregada de
`%LOCALAPPDATA%\LATRIB\supervisorio_settings.json`, chave `relacao`. Na
configuracao atual, `i=4`, portanto 1 RPM no encoder gera comando de 4 RPM no
motor. O valor e mostrado antes da autorizacao de movimento e pode ser
alterado no menu.

Sequencia:

1. o operador autoriza o movimento;
2. `a5_speed_logger` configura o Drive e responde `READY`, ainda parado;
3. o DLG aplica o preset do CH3, tenta confirma-lo por readback e exige dados;
4. somente depois o programa envia `START` ao Drive;
5. apos `STARTED`, descarta o backlog DLG anterior ao movimento e reancora o
   relogio da captura em pacotes atuais;
6. detecta quatro wraps, que delimitam tres voltas completas;
7. solicita `STOP` ao Drive e exige a confirmacao `STOPPED`;
8. fecha os CSVs e executa a regressao;
9. grava o JSON somente se todas as validacoes forem aprovadas.

O primeiro trecho parcial, entre a partida e o primeiro wrap, nao entra no
ajuste. Os intervalos entre os quatro wraps formam as tres voltas completas.

O DLG trabalha por padrao a 200 Hz. A 1 RPM isso fornece aproximadamente
12.000 amostras por volta, ou 0,03 grau nominal por amostra. O Drive registra
`P0B-09` a 10 Hz para nao voltar a sobrecarregar o Modbus. Essa taxa fornece
uma referencia a cada 0,6 grau do eixo do encoder e e suficiente para o
movimento lento. Nos registros validos da calibracao, o `t_qpc` do Drive e o
ponto medio real da transacao Modbus; linhas `NULL` preservam o deadline
nominal apenas para documentar a perda.

### Referencia angular e regressao

O processamento:

1. le as linhas DLG validas e classifica cada linha do Drive como valida,
   ausente, invalida ou outlier;
2. usa o `pos_mod` gravado pelo Drive, inclusive quando `P05-02` nao e 65536;
3. desenrola a posicao nos dois sentidos;
4. rejeita saltos numericos que excedam a velocidade fisicamente possivel
   para o RPM e a relacao configurados, assim como recuos maiores que a
   pequena banda admitida para jitter;
5. interpola `P0B-09` no QPC de cada amostra DLG somente em lacunas de ate
   0,5 s;
6. calcula a relacao mecanica medida em cada volta;
7. converte o progresso do Drive em referencia angular;
8. exclui 5 graus em cada lado do wrap;
9. divide cada volta em bins de 1 grau;
10. exige pelo menos 8 amostras e usa a mediana raw de cada bin;
11. exige pelo menos 320 bins validos em cada volta;
12. ajusta uma regressao linear robusta Huber nas duas primeiras voltas;
13. valida a terceira volta sem usa-la no treinamento;
14. depois da aprovacao, reajusta o modelo operacional com as tres voltas.

Perdas `NULL` ou linhas com `pos_err` nao recebem copia do ultimo valor. O
algoritmo usa os pontos validos anterior e posterior apenas quando a lacuna
tem no maximo 0,5 s; regioes maiores ficam sem referencia e sao descartadas.
O limite e temporal, portanto nao muda de significado se a taxa do Drive for
alterada. Um valor numerico isolado, mas fisicamente impossivel, tambem e
descartado e o ponto valido seguinte pode recuperar a sequencia. A relacao
medida deve ficar a no maximo 1% do valor configurado em cada volta.

Esse tratamento e um filtro de qualidade da referencia do Drive, nao um LPF
do sinal analogico. Ele evita inventar posicoes e evita atraso de fase. O
filtro operacional do encoder continua sendo a mediana causal de 9 amostras.

### Criterios de aprovacao

A terceira volta e avaliada de duas formas:

- medianas por bin de 1 grau;
- todas as amostras depois da mesma mediana causal de 9 amostras usada pelo
  monitor.

As duas avaliacoes devem atender:

| Metrica | Limite |
|---|---:|
| RMSE | <= 0,5 grau |
| P95 do erro absoluto | <= 1,0 grau |
| Erro absoluto maximo | <= 2,0 graus |
| Erro maximo da relacao mecanica | <= 1% |
| Saturacao A/D | nenhuma |

Os erros angulares sao circulares; por exemplo, 359 e 1 graus diferem por
2 graus, nao por 358.

Uma reprova:

- preserva o JSON de calibracao anterior;
- preserva os CSVs DLG/Drive;
- preserva o log completo;
- mostra separadamente as metricas por bin e com mediana de 9.

Isso evita aprovar um sinal cuja mediana por bin pareca boa, mas que flutue
demais em cada instante.

### Arquivos

Com a saida historica `encoder_CH3_mA.json`, o nome permanece por
compatibilidade, mas o conteudo aprovado da opcao 2 declara:

```json
"purpose": "encoder_ch3_angle_deg",
"unit": "deg"
```

O campo `unit`, e nao o nome do arquivo, define o tipo da calibracao.

Artefatos:

```text
encoder_CH3_mA.json
encoder_CH3_mA_autocal.csv
encoder_CH3_mA_autocal_events.log
encoder_CH3_mA_autocal_drive\drive.csv
encoder_CH3_mA_autocal_drive\schedule.csv
encoder_CH3_mA_autocal_drive\a5_speed_events.log
```

O JSON registra preset do CH3, fit operacional, fit de treino, metricas do
holdout, `pos_mod`, relacao configurada e medida, saturacoes e perdas. Tambem
registra totais de linhas validas, ausentes, invalidas e outliers do Drive,
alem da maior lacuna observada. `configuration_readback_verified` fica
verdadeiro somente quando o `GETCHCFG` confirmou campo a campo; no modo
compativel o JSON declara `FALLBACK_ACQDATA`. A gravacao e atomica.

O log de eventos registra uma linha `DLG_HANDSHAKE` por tentativa, com modo
`STRICT` ou `FALLBACK`, comandos enviados, ACK/readback e seus valores reais,
respostas de `ACQSETUP`/`ACQSTART`, taxa observada, frames rejeitados,
quantidade de `ACQDATA`, erro de socket e eventual codigo de rejeicao.
`DLG_RESYNC` informa quantos pacotes pre-movimento foram descartados.
`DLG_READY` e `DLG_NOT_READY` repetem o estado final para que uma falha de
comunicacao possa ser diagnosticada sem depender da linha dinamica do console.

Eventos principais:

- `MOTION_CONFIG`, `DRIVE_READY`, `DRIVE_STARTED`, `DLG_RESYNC`;
- `STATE`, `CANDIDATE_START`, `WRAP_ACCEPT`, `REARM`;
- `REFERENCE_TAIL`: pequena cauda apos o quarto wrap para interpolar o Drive;
- `GAP_TOLERATED`, `GAP_RESET`, `REORDER_DROP`;
- `DRIVE_STATUS_TIMEOUT`, `DRIVE_COMM_LOST`;
- `ANGULAR_FIT`, com todas as metricas;
- `VALIDATION_FAILED`, com o motivo da reprova;
- `SUCCESS` e `END`.

## Monitoramento

A opcao 1 usa a mediana das ultimas nove amostras e atualiza uma unica linha
no maximo duas vezes por segundo.

Com JSON angular:

- aplica o fit `raw -> graus` diretamente;
- normaliza em `[0, 360)`;
- usa a mediana causal de 9 amostras para rejeitar picos do sinal analogico;
- atualiza quando o angulo muda em 0,01 grau;
- mostra `mA nominal` apenas como equivalente da faixa angular.

Depois que o JSON angular foi aprovado, o monitor nao consulta o Drive. As
perdas de `P0B-09` afetam somente a referencia temporaria usada para criar a
calibracao; a posicao operacional vem diretamente do CH3.

`mA nominal` nao e uma medicao eletrica independente. Para medir a corrente
real, use instrumento de loop apropriado.

Com uma calibracao manual em mA, o monitor mantem o caminho legado
`raw -> mA -> angulo nominal` e identifica o angulo como nominal.

O zero produzido pela autocalibracao e o minimo eletrico do encoder. Ele nao
define o zero mecanico da maquina; essa referencia devera ser tratada
separadamente.

## Diagnostico manual e calibracao com referencia

A opcao 3 mantem o detector de wraps sem o Drive. Ela espera quatro
transicoes e produz apenas uma normalizacao nominal dos extremos para 4 e
20 mA. Nao ha referencia angular independente, portanto esse modo nao
substitui a regressao da opcao 2.

A opcao 4 calibra dois pontos de corrente informados pelo operador. Ela pode
ser metrologica somente quando a corrente de referencia for medida ou gerada
por instrumento rastreavel.

Calibracoes antigas em ganho x1, x10 ou qualquer preset diferente de x3 sao
rejeitadas pelo loader, pois a escala raw muda com o ganho. E necessario
executar uma nova autocalibracao depois desta alteracao.

## Replay sem hardware

Uma captura motorizada pode ser reprocessada sem abrir UDP ou Modbus:

```text
dlg_encoder_test.exe --replay-autocal caminho\captura_autocal.csv --rate 200 --ratio 4
```

Se `--ratio` for omitido, o programa tenta carregar a configuracao atual do
supervisorio. Para auditoria historica, prefira informar a relacao usada na
captura. O replay procura automaticamente o `drive.csv` irmao:

```text
captura_autocal_drive\drive.csv
```

Capturas antigas com apenas tres wraps possuem somente duas voltas completas.
Elas podem ajudar no diagnostico externo, mas nao atendem ao holdout exigido e
nao sao aprovadas pela metodologia nova.

## Ligacao no CH3

O manual do DLG confirma o uso dos pinos 8 e 1 do DB9 do proprio CH3 para
entrada de corrente. Ele nao apresenta uma tabela que escreva explicitamente
`I+` e `I-`. A interpretacao adotada para validacao e:

- pino 8: `I_IN`, lado positivo;
- pino 1: retorno `II`, lado negativo.

Essa polaridade e uma inferencia tecnica. Valide primeiro com um calibrador
4-20 mA limitado ou confirme com a Lynx.

DB9 femea, olhando a face onde o plugue encaixa:

```text
        5   4   3   2   1
          9   8   7   6
```

A vista pelo lado da solda e espelhada.

### Variante de quatro fios

```text
Fonte externa isolada +12-24 V -> vermelho
Fonte externa isolada 0 V      -> preto
Encoder cinza, sinal 4-20 mA + -> CH3 pino 8
Encoder marrom, sinal 4-20 mA - -> CH3 pino 1
```

### Variante de tres fios

```text
Fonte externa isolada +12-24 V -> vermelho
Fonte externa isolada 0 V      -> preto
Encoder cinza, sinal 4-20 mA + -> CH3 pino 8
CH3 pino 1                     -> preto / 0 V da fonte
```

Fios de funcao:

- laranja: `SETH`/direcao; manter isolado;
- amarelo: `SETL`/reset; manter isolado.

Use fonte externa isolada de 12-24 Vcc, dimensionada conforme o manual do
encoder. Nao alimente o encoder pela excitacao/porta do canal do DLG: ela nao
foi documentada para fornecer a corrente de alimentacao exigida pelo encoder.
A excitacao de 2,5 V e uma configuracao do front-end do canal, nao uma fonte
de 12-24 V para o sensor.

Desenergize antes de alterar fios. Nunca ligue simultaneamente a saida ativa
do encoder e um calibrador ativo.

## Observacao sobre os testes anteriores

As capturas antigas foram feitas com outro preset e mostraram grande
flutuacao. A correlacao externa confirmou que os wraps correspondiam a cerca
de quatro voltas do motor por volta do encoder, coerente com `i=4`, mas o erro
angular era muito maior que o aceitavel. Esses resultados nao devem ser usados
como calibracao.

O proximo ensaio de bancada deve usar o preset x3/2,5 V/DC conectado/LPF 0.
O programa reprovara explicitamente qualquer saturacao. As metricas do
holdout dirao se a estabilidade observada na Lynx tambem foi reproduzida pelo
caminho UDP, enquanto os contadores do Drive mostrarao quantas perdas e
leituras anormais foram filtradas.

## Compilacao

Na raiz do repositorio:

```powershell
$cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake --build .\DLG4000\build_vs2022 --config Release --target dlg_encoder_test
```

Saida:

```text
DLG4000\bin\Release\dlg_encoder_test.exe
```

Para o modo motorizado, distribua tambem `a5_speed_logger.exe` e a DLL
Release do libmodbus ao lado dele, ou preserve a estrutura do repositorio.
