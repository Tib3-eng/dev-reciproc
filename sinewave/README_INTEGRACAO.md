# Manual de integracao com o supervisorio

Este manual acompanha a primeira versao da integracao entre o supervisorio
DevReciproc e o programa de curvas multissegmento do colega.

**Situacao atual:** a interface, a comunicacao entre processos e os arquivos
podem ser exercitados em simulacao. O movimento real esta bloqueado porque
o gerador recebido ainda precisa de correcao e validacao de bancada. A
simulacao usa uma senoide de referencia separada e nao comprova a curva do
programa recebido. O controle pretendido e de **posicao de ida e volta**,
executado pelo Drive em **modo velocidade com multiplos segmentos**.

## Primeiro teste pela interface

1. Abra o supervisorio e preencha a identificacao do ensaio, o raio e a forca
   normal. Confira a relacao mecanica nas configuracoes adicionais.
2. Marque **Senoidal (teste)**. A tabela inferior de velocidade/distancia
   sera substituida pelos parametros da curva.
3. Mantenha **Simular sem hardware (dados sinteticos)** marcado.
4. Como exemplo, use curso de 4 mm, frequencia de 0,2 Hz e 2 ciclos.
   Com raio de 10 mm e relacao 4, a previsao sera 10 s, 16 mm e pico de
   9,6 RPM no motor. Cada ciclo significa uma ida e uma volta.
5. Clique em **Iniciar**. Acompanhe os graficos e aguarde a finalizacao dos
   arquivos. **Parar** encerra antecipadamente e preserva os dados parciais.
6. Confira a indicacao de simulacao no _I e na imagem _G. Desmarcar o modo
   senoidal devolve a tabela de etapas, quando nao houver operacao ativa.

O controlador e localizado na pasta `controladores` ao lado do supervisorio
compilado. **Selecionar controlador...** permite escolher outro executavel
compativel. No ambiente de fontes, existe fallback para o script Python
quando nenhum executavel foi encontrado.

Pausa/retomada nao esta disponivel neste perfil. A troca de modo e bloqueada
durante ensaio, tara, monitoramento e finalizacao.

## Parametros enviados e responsabilidade de cada programa

| Parametro | Unidade / significado | Quem fornece |
|---|---|---|
| `curve.course_mm` | Curso completo entre extremos, pico a pico, em mm | Painel senoidal |
| `curve.frequency_hz` | Ciclos completos por segundo | Painel senoidal |
| `curve.cycles` | Numero inteiro de ciclos; um ciclo = ida + volta | Painel senoidal |
| `mechanics.radius_mm` | Raio da pista do pino, em mm | Supervisorio |
| `mechanics.ratio` | i = D2/D1, polia do disco / polia do motor | Configuracoes adicionais |
| `serial` | Porta, baud, paridade e endereco Modbus | Adaptador; padrao COM5/115200/8E1, endereco 1 |
| `force_normal_n` | Forca normal em N, para calcular CoF | Supervisorio |
| `distance_interval_mm` | Intervalo da agregacao por distancia | Configuracoes adicionais |
| `output.directory` | Pasta absoluta DadosDev desta execucao | Supervisorio |
| `run_id` | Identificador unico da execucao | Supervisorio |
| `simulation` | Declaracao explicita de simulacao ou hardware | Painel senoidal |
| `safety` | Watchdog, idade maxima e limite de curso | Contrato v1 |
| `encoder_feedback` | Endereco UDP local, sessao e relogio do CH3 | Adaptador, somente no hardware |

O programa do colega recebe apenas as grandezas derivadas necessarias ao
gerador: `peak_motor_rpm` e `period_s`. Para curso C, frequencia f, raio r e
relacao i, o adaptador usa:

```
period_s        = 1 / f
duration_s      = cycles / f
distance_mm     = 2 * C * cycles
peak_speed_mm_s = pi * C * f
peak_motor_rpm  = 30 * i * C * f / r
```

Isso corresponde a posicao senoidal com amplitude C/2; velocidade e a
derivada da posicao. As unidades de tempo e velocidade codificadas nos
registradores do Drive ainda precisam ser verificadas pelo colega.
Limites atuais do pedido: curso/raio/frequencia/relacao positivos e finitos,
ciclos inteiros de 1 a 1000000, duracao nominal ate sete dias e pico calculado
ate 6000 RPM. Esse teto de pedido nao valida a capacidade mecanica da bancada.

O supervisorio cuida da interface, aquisicao DLG, calibracao, graficos e
relatorios. O runtime externo cuida da vida do processo, protocolo e enlace
do encoder. O colega cuida da formula, segmentos e comandos do seu Drive.
O logger de controle `a5_speed_logger` nao atua em paralelo com o controlador
senoidal durante o ensaio. A fase preliminar legada, quando aplicavel, termina
antes de iniciar o controlador externo.

## Mapa dos arquivos

| Arquivo | Funcao |
|---|---|
| `sinewave/curve_program.py` | Area comentada do gerador, preparada para o colega editar |
| `sinewave/motor.py` | Classe do colega; comunicacao, segmentos e comandos do Drive |
| `sinewave/sinewave_runtime.py` | Executavel externo: CLI, JSONL, simulacao e encerramento |
| `Supervisório/sinewave_protocol.py` | Versao do contrato, unidades, conversoes e pacote do encoder |
| `Supervisório/sinewave_integration.py` | Processo externo e adaptacao aos arquivos do supervisorio |
| `Supervisório/sinewave_ui.py` | Checkbox, campos e callbacks da interface |
| `Supervisório/tests/test_sinewave_integration.py` | Testes sem hardware |
| `sinewave/examples/simulacao.json` | Exemplo completo do pedido v1 |

O modo **Senoidal (teste)** substitui a tabela inferior por curso pico a pico
(mm), frequencia (Hz) e ciclos (ida + volta). Raio e relacao mecanica continuam
no supervisorio. **Simular sem hardware** vem marcado e gera dados sinteticos
identificados na tela, no _I e no PNG. Nao abre COM nem inicia DLG.

## Onde o colega altera a curva

- `curve_program.py`, funcao `plan_curve`: formula e segmentos/quadrantes.
- `motor.py`, metodo `vel_ctrl`: envio da tabela ao Drive, como antes.
- A formula recebida foi preservada; a integracao nao a corrige silenciosamente.
- `HARDWARE_VALIDATED=False` bloqueia movimento real. Nao basta trocar essa flag:
  a validacao tambem exige sinais opostos, tempos codificaveis e area zero.
  Confirmar a escala de tempo/rampas do A5 e STOP antes de validar hardware.

As entradas da curva sao `peak_motor_rpm` (RPM NO MOTOR) e `period_s` (ciclo
completo). O adaptador deriva esses valores do curso, frequencia, raio e relacao.
Retornar parametros/tabela efetivos permite registrar o que foi configurado.
Se forem necessarias recargas durante movimento, combinar antes uma operacao
curta de atualizacao; um loop bloqueante impediria atender STOP.

## O que foi adaptado em motor.py

O exemplo manual foi isolado por `if __name__ == '__main__'`. O construtor
aceita comunicacao configuravel; as operacoes usadas pelo runtime aplicam
`device_id` e propagam erro. Setup da fonte/cabecalho e STOP possuem readback.
A geracao das velocidades e dos tempos no `vel_ctrl` permanece a recebida.
Nao foram migrados/corrigidos os caminhos analogico e de posicao.

## Arquivos nossos

`sinewave_runtime.py` cuida de CLI, JSON por linha, simulacao e vida do processo.
No supervisorio, `sinewave_ui.py` contem os widgets/callbacks, e
`sinewave_integration.py` cuida do processo e dos dados. O protocolo de 92 bytes
do CH3 e lido por `sinewave_protocol.py`; nao e responsabilidade do gerador.

O hardware passa por validacao sem COM antes da tara. Depois usa calibracao
angular, tara e fase preliminar existentes, em sequencia. A aplicabilidade do
offset legado a senoide precisa de validacao experimental e fica identificada.
Nao usar as inversoes do controlador atual junto da tabela senoidal.

## Contrato e execucao

O supervisorio grava `DadosDev/sinewave_request.json` e chama:

```
sinewave_controller.exe --config "caminho/sinewave_request.json" --ipc
```

STDIN recebe JSONL com `schema_version=1`, `run_id`, `type` e `request_id`.
Comandos de movimento: `start` e `stop`. Sem START nao ha movimento. EOF
solicita parada. Eventos: `ready`, `started`, `status`, `physical_extreme`,
`failed` e `stopped`. STDOUT e exclusivo do protocolo; prints vao para STDERR.
Pausa/retomada nao sao suportadas nesta versao. O processo encerra apos STOPPED.

`--describe` e `--config ... --validate` inspecionam o contrato sem abrir COM.
O supervisorio oferece selecao de executavel, permitindo atualizar o programa
externo sem recompilar a UI quando o contrato permanece compativel.

### Exemplo de pedido e mensagens

O arquivo [examples/simulacao.json](examples/simulacao.json) contem um pedido
completo. Antes de usa-lo fora da UI, ajuste `output.directory` para uma pasta
absoluta gravavel. O JSON nao expande variaveis como `%USERPROFILE%`.
Na interface o supervisorio cria o pedido automaticamente, com um novo
`run_id` por execucao; nao e necessario editar esse arquivo manualmente.

Cada linha de STDIN e um objeto JSON completo, terminado por uma quebra de
linha. Estes exemplos usam o `run_id` do arquivo de demonstracao:

```json
{"schema_version":1,"run_id":"exemplo-sim-senoidal-v1","type":"start","request_id":"start-1"}
{"schema_version":1,"run_id":"exemplo-sim-senoidal-v1","type":"stop","request_id":"stop-1","reason":"operator"}
```

Na operacao normal, aguarde `ready`, envie `start`, aguarde `started` e
consuma os eventos ate `stopped`. Versao ou sessao divergente causa falha;
nao se deve reenviar START de uma execucao anterior.

| Evento | Significado |
|---|---|
| `ready` | Preparacao concluida; ainda sem movimento |
| `started` | Comando de partida aceito; inicio da execucao |
| `status` | Progresso e duracao para a interface |
| `physical_extreme` | Extremo medido, com stroke, posicao e QPC da amostra |
| `failed` | Erro com codigo/mensagem; deve iniciar o encerramento |
| `stopped` | Resultado final, motivo e confirmacao dos comandos de parada |

Os eventos incluem `schema_version`, `run_id`, `type` e `qpc`. Comandos
trazem `request_id`; os eventos de partida/parada podem correlaciona-lo.
`command_confirmed` indica o resultado do comando: nao deve ser interpretado
como prova independente de repouso fisico. Em hardware, `feedback_stationary`
permanece sem confirmacao nesta versao; esse ponto exige validacao de bancada.

Fechar STDIN solicita STOP. Se o controlador nao responder, o supervisorio
registra que a parada nao foi confirmada; matar o processo nao comprova que
o Drive parou. Nenhum diagnostico textual deve ser escrito em STDOUT, pois
isso quebra o leitor JSONL. Direcione mensagens humanas para STDERR.

Dados sinteticos ficam em `DadosDev/dlg.csv` e `encoder_state.csv`, 200 Hz,
indices/QPC identicos. No hardware estes arquivos continuam sendo do DLG real.
Os arquivos finais _T/_DP/_M/_I e _G sao gerados pelo supervisorio. Nao se cria
telemetria falsa do Drive para satisfazer o merge antigo. Em _M,
VELOCIDADE_ALVO e NULL porque a referencia nao e constante dentro do stroke.
Parada parcial preserva _T e nao contabiliza stroke incompleto como concluido.

### Arquivos tecnicos para diagnostico

Na pasta `DadosDev` ficam `sinewave_request.json`, `sinewave_effective.json`,
`controller_events.jsonl`, `controller_stderr.log`, `dlg.csv` e
`encoder_state.csv`. No hardware tambem pode existir `dlg_stdout.log`.
O pedido registra o solicitado; `sinewave_effective.json` registra a tabela
efetivamente preparada ou identifica o modelo de simulacao.

Os arquivos finais _T/_DP/_M/_I e _G so sao montados quando o supervisorio
orquestra o ensaio; chamar o runtime isoladamente gera os arquivos tecnicos.
O _I identifica a versao do gerador, simulacao e uso do offset preliminar.

## Executar pelo codigo-fonte, sem hardware

Use Windows e Python 3.12. Da raiz do repositorio, instale as dependencias
do controlador e inspecione suas capacidades:

```powershell
python -m pip install -r .\sinewave\requirements.txt
python .\sinewave\sinewave_runtime.py --describe
python .\sinewave\sinewave_runtime.py --config .\sinewave\examples\simulacao.json --validate
```

Depois de ajustar a pasta de saida no exemplo, execute:

```powershell
python .\sinewave\sinewave_runtime.py --config .\sinewave\examples\simulacao.json --ipc
```

Ao receber `ready`, digite a linha JSON de `start` acima. O exemplo completa
dois ciclos em dez segundos; tambem e possivel enviar o JSON de `stop` antes.
Existe um prazo de 30 s para receber START. Esta execucao esta em simulacao,
nao abre a porta serial e nao precisa de DLG.

Para verificar o contrato e os arquivos sem iniciar a interface:

```powershell
python -m unittest discover -s ".\Supervisório\tests" -p "test_sinewave_integration.py" -v
```

O conjunto da integracao usa simulacao e mocks Modbus. A execucao completa
do conjunto de testes do supervisorio usa `-p "test_*.py"`. Na verificacao
de 29/10/2026: 68 testes executados, sem falhas, com quatro skips por
dependencias/fixtures opcionais preexistentes. Nenhum teste desta integracao
exige movimento real.

## Build Windows

As versoes verificadas estao em `requirements.txt` e
`requirements-build.txt`. Para compilar o controlador, da raiz do repositorio:

```powershell
python -m pip install -r .\sinewave\requirements-build.txt
python -m PyInstaller --noconfirm --distpath sinewave/dist --workpath out/sinewave_build sinewave/build_py/sinewave_controller.spec
New-Item -ItemType Directory -Force -Path ".\Supervisório\dist\controladores" | Out-Null
Copy-Item -LiteralPath .\sinewave\dist\sinewave_controller.exe -Destination ".\Supervisório\dist\controladores\sinewave_controller.exe" -Force
```

Distribuir `sinewave_controller.exe` na pasta `controladores` ao lado do
`novo_tribometro.exe`. A UI permanece no executavel oficial; o controlador e
um executavel autocontido separado. Fechar o processo nao substitui a parada
do Drive: perda abrupta do PC e watchdog do equipamento exigem validacao fisica.

Para recompilar tambem o supervisorio, use o spec existente. Suas demais
dependencias devem estar instaladas; `sinewave/requirements.txt` e apenas
do controlador. O ambiente verificado usa matplotlib 3.10.8 e openpyxl 3.1.5.

```powershell
Push-Location ".\Supervisório\build_py"
python -m PyInstaller --noconfirm --distpath ..\dist --workpath ..\..\out\supervisor_build novo_tribometro.spec
Pop-Location
```

Estrutura minima adicional para distribuir esta integracao:

```
Supervisório/dist/
  novo_tribometro.exe
  controladores/
    sinewave_controller.exe
```

Os executaveis C e calibracoes necessarios aos ensaios reais continuam
obrigatorios. Os `.exe`, logs e capturas locais nao fazem parte deste commit;
os fontes e specs permitem gerar a distribuicao. Nao executar `motor.py`
diretamente como substituto do runtime: seu bloco manual e um exemplo de
bancada e pode configurar/iniciar o Drive, com constantes proprias.

## Como o colega deve continuar a implementacao

1. Altere a formula e a divisao em segmentos/quadrantes em `plan_curve`.
   Para o pedido atual, o perfil deve produzir ida e volta, com deslocamento
   final proximo ao inicial. A divisao em quatro quartos pode ser feita sem
   alterar o contrato entre os programas.
2. Revise em `Motor.vel_ctrl` a escala real de tempo, os limites da tabela,
   a codificacao dos valores negativos e as rampas do A5. Hoje a validacao
   rejeita tempos fracionarios porque o caminho recebido usa `int(dt)`.
3. Mantenha `prepare_curve` sem partida. Retorne a tabela efetiva e propague
   erros de comunicacao. Mantenha `start_curve` e `stop_curve` curtos, para
   que o adaptador consiga atender encerramento. STOP deve funcionar depois
   de configuracao parcial e poder ser repetido.
4. Verifique todos os modos de encerramento na bancada: fim de ciclo, STOP,
   EOF, perda de feedback, falha de comunicacao e watchdog. Confirmar comando
   Modbus e confirmar o repouso fisico sao verificacoes distintas.
5. Valide a trajetoria medida pelo CH3, os extremos, o limite de 2x o curso,
   idade maxima de 100 ms, ultima parada e a aplicabilidade do offset legado.
   Apenas depois dessa validacao atualize a versao do gerador e o estado
   `HARDWARE_VALIDATED`. Mudar a flag isoladamente nao habilita uma curva
   rejeitada pelos outros testes do plano.
6. Recompile somente `sinewave_controller.exe` e substitua-o na pasta
   `controladores`, com os programas fechados. Sem mudanca de contrato,
   nao e necessario recompilar a interface para cada ajuste da formula.

Um parametro novo deve entrar de forma explicita no pedido, no painel e no
registro efetivo; nao depender de edicao manual de constantes escondidas.
Se o gerador precisar recarregar segmentos durante o movimento, sera preciso
definir uma operacao curta de atualizacao antes de estender o contrato.

## Problemas frequentes

| Sintoma | Conferencia |
|---|---|
| Controlador nao encontrado | Conferir a pasta `controladores` ou selecionar o exe no painel |
| Rejeicao de hardware / dois sentidos / tempos fracionarios | Limitacao conhecida do gerador recebido; manter simulacao ate o colega corrigir e validar |
| Versao ou sessao incompativel | Mesmo contrato v1 e mesmo `run_id` no pedido, comandos e eventos |
| JSON invalido em STDOUT | Remover prints de diagnostico desse canal e usar STDERR |
| Sem arquivos finais ao rodar o runtime isolado | Abrir o ensaio pela UI para gerar _T/_DP/_M/_I/_G |
| Falha no setup ou na parada | Consultar `controller_stderr.log` e `controller_events.jsonl`; nao classificar falha como sucesso |

As rotinas legadas de controle analogico/posicao do arquivo recebido nao
foram migradas. A integracao publicada usa exclusivamente o caminho de
velocidade multissegmento e o contrato descrito neste manual.
