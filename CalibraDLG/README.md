# CalibraDLG

Objetivo: calibrar um canal do DLG4000 com media de 1 s por ponto e gerar ajuste linear.

## Fluxo
O executavel faz perguntas e realiza a leitura direta do DLG4000:
1) Qual canal (CH1..CH8).
2) Tipo de sensor (tSensor).
3) Quantos pontos.
4) Para cada ponto, informe o valor de referencia e o programa captura media de 1 s do canal.

## Saida
JSON ASCII em `out/` com os pontos e o ajuste.
O cabecalho inclui a lista de canais calibrados:

out/calib.json

## Exemplos
Executar:
CalibraDLG.exe
