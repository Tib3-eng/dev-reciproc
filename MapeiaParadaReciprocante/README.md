# Mapeamento da parada reciprocante

Programa separado para criar uma base de dados da resposta de parada entre
1 e 20 mm/s e cursos entre 4 e 50 mm. Ele nao altera o metodo operacional do
supervisorio: o Drive continua sendo controlado pelo logger C e o encoder
externo do DLG e usado para medir os gatilhos, extremos e distancias.

## Seguranca e sequencia

- O logger C exige parada reforcada, readback dos sete parametros do modo
  velocidade, tres leituras consecutivas de P0B-09 e P0B-34 sem falha antes
  de emitir READY.
- O DLG precisa entregar dados validos antes de o Drive receber START.
- A matriz sempre inicia em 1 mm/s e curso de 50 mm. Em cada velocidade, os
  cursos sao reduzidos de 50 para 30, 15 e 4 mm.
- Cada condicao faz seis strokes. O primeiro ciclo completo (dois strokes) e
  descartado da modelagem.
- Perda DLG acima de 1%, falha do encoder sombra, falta de ambos os sentidos,
  parada maior que um curso ou curso fisico de 2x interrompem a matriz.
- Latencia media encoder->controle acima de 20 ms ou mais de 2% dos pacotes
  acima de 100 ms interrompem. Entre 0,5% e 2% vira aviso de picos isolados,
  pois a posicao/extremo ainda e reconstruida do CSV completo do DLG.
- Se a parada superar 35% do curso, os cursos menores daquela velocidade sao
  pulados. O limite independente de 2x o curso permanece no logger C.
- Falta de pontos completos torna a condicao invalida para modelagem e bloqueia
  cursos menores naquela velocidade, mas nao impede recomecar o proximo degrau
  pelo curso de 50 mm. Falhas de seguranca interrompem toda a matriz.
- Ctrl+C envia STOP aos dois processos. A parada de emergencia fisica deve
  permanecer acessivel.

## Matriz padrao

Velocidades: 1, 2, 5, 10, 15 e 20 mm/s.

Cursos: 50, 30, 15 e 4 mm.

Isso forma ate 24 condicoes, executadas gradualmente. Regioes reprovadas nao
sao extrapoladas automaticamente.

## Relatorio

O programa grava em `out/stopping_curve/study_<data_hora>`:

- uma pasta auditavel por condicao;
- `pontos_parada_consolidados.csv`;
- `modelo_parada.json`;
- `RELATORIO_MAPA_PARADA.md`.

Sao comparados: tempo de resposta proporcional a velocidade, modelo fisico
com termos `v` e `v^2`, e superficie com velocidade, curso e interacao. A
validacao cruzada remove uma condicao completa por vez. O metodo escolhido e
o mais simples dentro de 10% do menor RMSE de validacao.

## Execucao

Abra `mapa_parada.py` e use o menu. Para automacao deliberada:

```powershell
python MapeiaParadaReciprocante/mapa_parada.py --run-default --yes
```

`--yes` declara que a area foi liberada para movimento. Sem ele, a execucao
por linha de comando e bloqueada.

O executavel pronto fica em `dist/mapa_parada_reciprocante.exe`. Para refazer
o pacote a partir da raiz do projeto:

```powershell
python -m PyInstaller --noconfirm --clean --distpath MapeiaParadaReciprocante/dist --workpath MapeiaParadaReciprocante/build MapeiaParadaReciprocante/mapa_parada_reciprocante.spec
```
