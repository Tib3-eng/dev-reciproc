# Prompt para o Codex — migração do programa principal para o encoder externo

> **Status em 17/08/2026:** este arquivo preserva o plano e as decisões que
> orientaram a migração; não representa mais uma lista de tarefas pendentes.
> O pipeline principal já usa CH3/EncoderCore a 200 Hz no contínuo e no
> reciprocante, sem polling periódico de posição/RPM do Drive. O controle
> reciprocante autoritativo e o último extremo estacionário foram validados em
> bancada entre 1 e 3 mm/s. Para o estado operacional, consultar
> `Supervisório/docs/aquisicao_200hz.md`,
> `Supervisório/docs/modo_continuo_encoder_externo.md` e
> `Supervisório/docs/controle_reciprocante_encoder_externo.md`.

Quero evoluir o projeto `dev-reciproc`, do tribômetro reciprocante do LATrib/UFRGS.

Repositório: `https://github.com/Tib3-eng/dev-reciproc`

## Objetivo geral

O encoder externo conectado ao CH3 do DLG deve passar a ser a única fonte de posição e movimento usada pelo programa principal para:

- distância e voltas;
- velocidade real do disco/pino;
- processamento por distância, volta ou stroke;
- detecção de extremos e inversões no modo reciprocante;
- decisão de conclusão do movimento, quando tecnicamente seguro.

O Drive continuará sendo usado para comandar o motor — habilitar, partir, aplicar velocidade/sentido, inverter e parar — mas o sistema não deve mais depender da leitura de `P0B-09`, RPM ou de `drive.csv` para calcular posição, processar o ensaio ou confirmar o movimento.

Também quero elevar a taxa do pipeline principal de aquisição de 50 Hz para 200 Hz.

Esta é uma alteração crítica, pois o software controla hardware real. Não faça uma grande reescrita de uma vez e não comece implementando antes de compreender o estado atual.

## Forma de trabalho obrigatória

1. Leia integralmente o `AGENTS.md` e examine o código atual, os testes, os documentos e o histórico Git relevante.
2. Localize todas as dependências de posição/RPM do Drive e todas as suposições de 50 Hz.
3. Examine também o `ReprocessadorEncoder`, que já reprocessa ensaios offline pelo encoder externo. Ele é uma referência funcional, mas não deve ser copiado cegamente para controle em tempo real.
4. Antes de editar qualquer arquivo, entregue:
   - um mapa do fluxo atual de aquisição, controle e processamento;
   - uma lista de arquivos/funções afetados;
   - as dependências diretas e indiretas de `P0B-09`, RPM, `drive.csv`, `merge_logs` e sincronização por índice;
   - as suposições fixas de 50 Hz, inclusive janelas expressas em número de amostras, timeouts, watchdogs, filtros e buffers;
   - as decisões em aberto e perguntas que precisam da minha resposta;
   - duas ou mais opções de arquitetura para levar o encoder do DLG ao controlador do Drive no modo reciprocante, com prós, contras, latência, complexidade e segurança;
   - sua recomendação técnica;
   - um plano revisado em tarefas pequenas, cada uma com alteração, validação, critério de aceite e possibilidade de rollback.
5. Na primeira resposta, faça somente essa auditoria e o plano. Não altere código.
6. Depois que eu aprovar, execute uma tarefa por vez. Ao final de cada tarefa:
   - compile e rode os testes aplicáveis;
   - mostre os arquivos alterados e resuma o diff;
   - apresente evidências dos testes;
   - informe riscos e pendências;
   - pare e aguarde minha aprovação antes da próxima tarefa.
7. Se uma hipótese importante não puder ser confirmada sem hardware, não a trate como verdadeira. Implemente teste, instrumentação ou log que permita validá-la na bancada.
8. Preserve as mudanças do usuário já existentes e não faça commit, push ou PR sem autorização explícita.

## Escopo funcional

### O que deve deixar de depender do Drive

- posição angular e posição acumulada;
- distância percorrida e número de voltas;
- velocidade medida;
- agrupamento por distância/volta/stroke;
- gráficos baseados em posição;
- arquivos finais de processamento;
- detecção de extremos e término de strokes reciprocantes;
- critério normal de distância concluída, se a arquitetura proposta demonstrar que isso é seguro.

### O que deve continuar no Drive

- comunicação necessária para enviar comandos ao inversor/servo;
- partida, parada, habilitação, velocidade e sentido;
- confirmação mínima de comando ou estado que seja indispensável à segurança, caso o protocolo exija.

Não confunda “retirar a leitura de posição/RPM do Drive” com “remover todo o controle do Drive”. Se houver leituras Modbus que precisem permanecer apenas para segurança, alarme, prontidão ou confirmação de comando, identifique-as separadamente e justifique.

## Plano inicial esperado

Você pode reorganizar as tarefas após a auditoria, mas a migração deve manter estes marcos separados:

### Tarefa 0 — Auditoria e decisões de arquitetura

- Mapear o estado atual e todas as dependências.
- Confirmar o significado exato de “sem leitura do Drive”.
- Definir a comunicação de baixa latência entre aquisição do CH3 e controle do motor.
- Definir política de falha segura.
- Definir contratos de dados, compatibilidade de CSV e critérios de aceite.
- Nenhuma alteração de código nesta tarefa.

### Tarefa 1 — Baseline e testes de replay

- Criar uma linha de base reproduzível antes da migração.
- Usar ensaios reais contínuos e reciprocantes, inclusive o ensaio em que o Modbus do Drive falhou mas o CH3 continuou válido.
- Criar testes para wrap `360° -> 0°`, sentido contrário, jitter, quarentena, gaps, encoder constante, saltos impossíveis e mudança de sentido.
- Comparar os resultados atuais e o `ReprocessadorEncoder` com valores esperados (“golden files” ou métricas equivalentes).
- Registrar distância, voltas, velocidade, quantidade de intervalos/strokes, extremos e perdas.

### Tarefa 2 — Núcleo único de estado do encoder

- Isolar uma lógica claramente testável para:
  - validação e calibração do CH3;
  - normalização angular;
  - unwrap com direção;
  - posição relativa ao início do ensaio;
  - distância incremental e acumulada;
  - velocidade com base temporal correta;
  - detecção de sentido, parada, extremos e sinal inválido;
  - qualidade/quarentena do sinal.
- Evitar duplicação divergente entre processamento ao vivo, processamento final e reprocessamento offline. Se compartilhar o mesmo código entre C e Python não for razoável, proponha uma especificação única e testes de paridade.
- Manter a distância do disco independente da relação mecânica. A relação `i` só deve entrar na conversão do comando de velocidade para o motor ou em grandezas explicitamente derivadas do motor.

### Tarefa 3 — Aquisição a 200 Hz

- Alterar somente o necessário para levar o pipeline de 50 Hz a 200 Hz.
- Auditar DLG, orquestrador, buffers, pacotes, QPC, duração, quantidade prevista de linhas, timeouts, watchdogs, pausa/retomada, logs, arquivos e consumo de CPU/disco.
- Converter filtros, histereses e timeouts que hoje dependam implicitamente de quantidade de amostras para critérios temporais, quando apropriado. Uma janela de 9 amostras ou confirmação de 5 amostras representa tempos muito diferentes em 50 e 200 Hz.
- Manter decimação apenas na visualização; processamento e arquivos devem conservar os dados completos.
- Validar primeiro sem alterar a fonte de posição do controle.

### Tarefa 4 — Modo contínuo somente pelo encoder externo

- Remover do modo contínuo a dependência de `P0B-09`, RPM e `drive.csv` para processamento.
- Gerar `_T` e os arquivos por distância/volta a partir do DLG/encoder.
- Calcular velocidade a partir de cruzamentos temporais interpolados ou método equivalente. Não usar apenas `distância do bloco / (tempo da última amostra - tempo da primeira)`, pois isso já produziu viés: intervalos de 4 mm chegaram a indicar aproximadamente 22,22 mm/s quando a velocidade global era cerca de 20,09 mm/s.
- Usar o encoder para o término normal por distância, se aprovado, mantendo um hard deadline independente como proteção contra encoder parado/inválido.
- Preservar pausa, retomada, parada manual, tara, monitoramento, etapas de velocidade e pós-captura.

### Tarefa 5 — Controle reciprocante pelo encoder externo

- Substituir `P0B-09` na máquina de estados reciprocante.
- A decisão de inversão deve usar dados causais do encoder com baixa latência e não pode depender de polling de arquivo CSV pela GUI como solução definitiva sem uma justificativa e medição de latência/jitter.
- Preservar a regra de velocidade travada durante o stroke e mudança de etapa somente na inversão seguinte.
- Tratar overshoot, desaceleração, inércia, ruído perto do extremo, histerese, debounce temporal, curso mínimo, timeout do stroke e dupla inversão.
- Preservar correção dinâmica, ciclo de estabilização descartado, pós-captura, recorte no último extremo e geração de `_M`/arquivo equivalente atual.
- Definir tolerância em milímetros no lado do disco, não em counts do encoder do Drive.

### Tarefa 6 — Remoção das dependências antigas

- Depois que contínuo e reciprocante estiverem validados, retirar polling de posição/RPM do Drive, merge dependente de `drive.csv`, caminhos mortos e artefatos desnecessários.
- Decidir comigo se `drive.csv` desaparecerá ou será substituído por um log somente de comandos/estados.
- Preservar compatibilidade com arquivos antigos quando necessário e documentar qualquer mudança de schema.
- Não manter colunas chamadas “RPM real” se forem apenas valores derivados do encoder; rotular claramente como velocidade do disco ou RPM equivalente calculado.

### Tarefa 7 — Validação integrada e documentação

- Testes unitários e de replay sem hardware.
- Testes de integração com processos simulados.
- Checklist de bancada em baixa velocidade antes de condições normais.
- Testes separados para contínuo, reciprocante, parada manual, pausa/retomada, perda do DLG, encoder constante, encoder intermitente, Drive sem resposta e encerramento normal.
- Comparação lado a lado com arquivos reais já validados.
- Atualização de `AGENTS.md`, documentação de arquitetura, formatos de arquivo e instruções de build/teste.

## Pontos de risco que precisam ser tratados explicitamente

1. **Arquitetura de controle:** hoje o processo do Drive conhece `P0B-09`, enquanto o CH3 pertence ao DLG. No reciprocante, alguém precisa transmitir posição/estado do encoder ao controlador com latência previsível. Compare IPC direto entre processos, controlador dedicado ou outra separação coerente. A GUI Tk/Python não deve se tornar um elo de tempo crítico sem evidência de que atende aos limites.
2. **Falha segura:** sinal constante, calibração ausente, saturação, gaps, excesso de quarentena, salto impossível ou ausência de movimento após comando devem provocar alarme e parada controlada. Interpolação pode ser aceitável no pós-processamento, mas não deve inventar movimento para decisões de segurança em tempo real.
3. **Wrap e jitter:** já houve movimento contrário acumulado de 1,312% e aproximadamente 9,807% de amostras em quarentena em um ensaio real, concentradas perto da transição angular. Pequenos backsteps não podem causar perda de distância nem inversões falsas.
4. **Mudança de 50 para 200 Hz:** quadruplicar a taxa altera memória, tamanho de arquivo, carga de CPU, cadência de UI, tempo representado por janelas em amostras e limites de velocidade angular por amostra.
5. **Velocidade:** a velocidade deve ser calculada com tempo monotônico/QPC e fronteiras interpoladas. Verifique unidades e evite o erro de `N` amostras conterem apenas `N-1` intervalos.
6. **Zero do encoder:** o zero elétrico absoluto não é o zero mecânico. Use origem relativa e explícita por ensaio/stroke.
7. **Relação mecânica:** o encoder está no disco; não divida a distância medida por `i`. Use `i` apenas para gerar o setpoint do motor ou grandezas motoras derivadas.
8. **Calibração:** o programa principal deve validar schema, unidade em graus, preset e qualidade da calibração do CH3 antes de permitir movimento dependente do encoder. Não faça fallback silencioso para sinal bruto sem unidade confirmada.
9. **Encerramento contínuo:** diferencie distância líquida, caminho total absoluto e pequenos retornos por ruído. Defina qual grandeza controla o alvo e como impedir que jitter antecipe ou atrase a parada.
10. **Extremos reciprocantes:** a reversão observada no encoder ocorre depois do comando ao Drive e inclui atraso/inércia. Diferencie ponto de comando, extremo físico e limite de segurança.
11. **Sincronização:** ao remover `drive.csv`, não preserve por acidente uma dependência de igualdade de índices entre dois loggers. O `_T` deve ter uma origem temporal clara.
12. **Compatibilidade:** inventarie consumidores dos CSVs, gráficos, nomes `_P/_DP`, `_VP`, `_M`, relatórios e scripts externos antes de mudar cabeçalhos ou remover colunas.
13. **Desempenho da interface:** a UI deve continuar responsiva. Limite a taxa de redesenho e desacople aquisição/processamento da renderização.
14. **Validação sem hardware:** se a compilação Windows ou o equipamento não estiverem disponíveis, ainda faça testes de parser, replay, máquina de estados e processos simulados; entregue um roteiro objetivo para os testes que dependem da bancada.

## Critérios gerais de conclusão

O trabalho só estará concluído quando:

- nenhum cálculo ou decisão de movimento depender de `P0B-09`, RPM lido do Drive ou `drive.csv`;
- o Drive continuar recebendo comandos com parada segura;
- o pipeline registrar 200 Hz de forma demonstrável, com métricas de taxa efetiva, jitter e perdas;
- os resultados do encoder coincidirem com os arquivos reais/golden dentro de tolerâncias justificadas;
- contínuo e reciprocante passarem pelos testes de replay e integração;
- falhas do encoder forem detectadas e não forem classificadas como sucesso;
- os arquivos finais e gráficos mantiverem consistência e compatibilidade documentada;
- cada etapa tiver evidência de compilação, testes e revisão do diff.

## Perguntas mínimas que espero na auditoria

Além das perguntas que surgirem no código, confirme comigo:

1. “Retirar a leitura do Drive” significa remover somente posição/RPM ou também qualquer leitura de status/alarme/confirmação?
2. No contínuo, o ensaio deve terminar prioritariamente pela distância do encoder, deixando o tempo apenas como hard deadline?
3. `drive.csv` deve desaparecer ou virar um log de comandos/estados sem posição?
4. Quais cabeçalhos e nomes de arquivos precisam permanecer compatíveis com ferramentas externas?
5. Quais tolerâncias de curso, overshoot, perda de amostras, jitter e tempo sem encoder são aceitáveis na bancada?
6. Há ensaios reais contínuos e reciprocantes adicionais que posso fornecer como golden files?

Comece agora somente pela Tarefa 0: auditoria, perguntas, opções de arquitetura e plano revisado. Não edite o código até eu aprovar.
# Registro historico: mapa empirico de parada (2026-08-13)

Foi criado o programa separado `MapeiaParadaReciprocante`, com interface de
menu e gates de hardware. A matriz 1..20 mm/s por cursos 4..50 mm foi executada
em 24 condicoes; 22 entraram no ajuste. O relatorio completo esta em
`out/stopping_curve/pilot_v1_c50/RELATORIO_MAPA_PARADA.md`.

O melhor modelo preliminar, validado removendo uma condicao completa por vez,
foi `distancia_parada_mm = 0.1319983583 * abs(velocidade_externa_mm_s)`, RMSE
0.1296 mm. Para avaliacao das medias balanceadas, o P95 unilateral e 0.21995
mm; para um stroke individual, usar provisoriamente a margem bruta P95 de
0.2702500163 mm (maximo bruto 0.33931 mm). O curso nao melhorou a
validacao depois de usar a velocidade externa real, mas strokes curtos podem
nao atingir a velocidade alvo. Naquele marco o modelo ainda era somente
diagnostico, com `action_enabled=0`. Essa restricao historica foi superada: o
modelo foi ativado no controlador autoritativo com OLS causal de 250 ms e
limite de 45% do curso e validado em bancada de 1 a 3 mm/s. Replay completou
22/24 condicoes, falhando somente nas capturas 15/4 e 20/4 ja excluidas do
ajuste; velocidades maiores continuam dependentes de validacao gradual no
programa principal.
