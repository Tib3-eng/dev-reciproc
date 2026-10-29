# DevReciproc

Controle e aquisicao de dados do tribometro: supervisorio Python/Tk,
DLG4000, calibracao e controle do Drive A5.

A integracao com o controlador de curvas do colega esta em
[`sinewave/`](sinewave/README_INTEGRACAO.md). O manual descreve o uso do
modo **Senoidal (teste)**, a comunicacao entre os programas, os parametros,
a geracao dos executaveis e os pontos que o colega deve alterar.

**Estado desta integracao:** simulacao disponivel; movimento real bloqueado
ate a correcao e validacao do gerador e da parada na bancada.

- [Manual de integracao e operacao](sinewave/README_INTEGRACAO.md)
- [Area comentada do gerador](sinewave/curve_program.py)
- [Exemplo de configuracao para simulacao](sinewave/examples/simulacao.json)
- [Testes da integracao](Supervis%C3%B3rio/tests/test_sinewave_integration.py)
