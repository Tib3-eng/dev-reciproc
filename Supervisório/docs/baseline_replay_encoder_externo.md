# Baseline de replay do encoder externo

Este documento registra a Tarefa 1 da migracao do programa principal para o
encoder externo. Nenhuma funcao de controle usa estes helpers: toda a
implementacao desta etapa fica em `tests/`.

## Dados reais congelados

O manifesto `tests/fixtures/encoder_real_baselines.json` identifica cada par
`_I/_T` por SHA-256. Isso impede que outro ensaio com o mesmo nome seja aceito
silenciosamente.

Os arquivos grandes nao foram copiados para o Git. O teste procura primeiro no
repositorio e depois em `Desktop/Repositorio`. Em outra maquina, defina
`DEVRECIPROC_REPLAY_DATA` para uma pasta com estas subpastas:

```text
continuous_modbus_loss/
  2026-08-06-XM19_ACO-2-1_I.csv
  2026-08-06-XM19_ACO-2-1_T.csv
reciprocating_multispeed/
  2026-08-05-TesteEncoder-2-1_I.csv
  2026-08-05-TesteEncoder-2-1_T.csv
```

### Continuo com perda Modbus

- programado: 70.000 mm a 20 mm/s;
- processamento antigo pelo Drive: 4.476 mm e 71 voltas;
- encoder externo: 70.304,983177 mm e 1.118,938560 voltas;
- erro contra a distancia programada: aproximadamente +0,436%;
- velocidade global pelo encoder: aproximadamente 20,088 mm/s;
- quarentena: 17.161 de 174.990 linhas;
- movimento contrario compensado: aproximadamente 1,312% do avanco.

Este ensaio comprova que o CH3 permaneceu utilizavel quando a telemetria de
posicao/RPM do Drive deixou de representar o movimento real.

### Reciprocante multivelocidade

- curso configurado: 10 mm;
- distancia programada: 1.920 mm;
- eventos do controlador atual: 183 inversoes mais `RECIP_DONE`, totalizando
  184 strokes fisicos;
- `_M` atual: 184 strokes;
- segmentacao do reprocessador temporario: 198 trechos detectados;
- somente os 192 primeiros sao escritos no `_M` por causa da quantidade
  programada, e nove deles medem menos de 75% do curso;
- a distancia de 1.921,916350 mm exibida no relatorio soma os 198 trechos,
  inclusive seis que nao aparecem no `_M`;
- entre todos os trechos ha 15 abaixo de 75% do curso, incluindo falsos strokes
  de aproximadamente 0,06 a 1,15 mm.

A contagem do reprocessador temporario nao e uma verdade de referencia para o
reciprocante. O baseline preserva separadamente eventos do controlador,
distancia externa e segmentacao offline para que a nova maquina de estados seja
validada sem esconder essa divergencia.

## Casos compactos

`tests/fixtures/encoder_replay_cases.json` cobre:

- wrap nos dois sentidos;
- jitter com eixo parado;
- quarentena operacional;
- amostras ausentes e gap consecutivo;
- salto fisicamente impossivel;
- mudanca confirmada de sentido;
- cruzamento temporal interpolado, evitando o erro de `N` pontos conterem
  somente `N-1` intervalos.

Esses casos serao reutilizados na Tarefa 2 para testes de paridade do novo
nucleo C. Interpolacao ou amostra reconstruida podera ser usada no resultado
offline, mas nao confirmara movimento para o controlador.

## Executar

Na pasta `Supervisório`:

```powershell
python -m unittest discover -s tests -v
```

Se os dados reais nao estiverem disponiveis, os casos grandes aparecem como
`skipped`; os casos compactos continuam obrigatorios.
