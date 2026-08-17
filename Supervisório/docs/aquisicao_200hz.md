# Aquisição principal a 200 Hz

## Pipeline atual

O programa principal usa o DLG a 200 Hz como fonte única de força, posição,
velocidade e distância. As requisições periódicas `P0B-09` e `P0B-00` foram
removidas tanto do contínuo quanto do reciprocante.

Arquivos técnicos:

- `dlg.csv`: CH1..CH8 a 200 Hz, sem filtro temporal destrutivo;
- `encoder_state.csv`: posição relativa, caminho, velocidade, RPM do disco,
  sentido, reversão, saúde e flags do encoder, também a 200 Hz;
- `drive.csv`: comandos enviados ao Drive; não contém telemetria de posição ou
  RPM;
- `dlg_compat_50hz.csv`: não é gerado no pipeline principal. Permanece apenas
  em ferramentas legadas/diagnósticas que ainda solicitarem compatibilidade.

O contínuo e o reciprocante geram os arquivos finais diretamente de
`dlg.csv + encoder_state.csv`. Não há merge por índice com o Drive, conversão
por relação mecânica nem interpolação de `P0B-09`.

## Temporização

- aquisição e estado do encoder: 200 Hz;
- confirmação do filtro de transição: 100 ms;
- histórico de velocidade do filtro: 300 ms;
- estimador de parada reciprocante: OLS causal de 250 ms;
- barreira de origem antes do movimento: até 3 s, com o Drive mantido parado;
- timeout sem encoder/movimento inicial: 3 s;
- pós-captura reciprocante: até 2 s para localizar o último extremo físico;
- registro de comandos do Drive: 50 Hz, sem transações periódicas de leitura.

No reciprocante, `DATA_OK` confirma que a aquisição do DLG está ativa, mas pode
ser emitido alguns milissegundos antes do primeiro pacote causal do EncoderCore
chegar ao processo do Drive. Por isso, após `START`, o `a5_speed_logger` espera
até 3 s por uma amostra CH3 aceita, inicializada e saudável. O motor permanece
parado durante essa barreira e o cronômetro do movimento começa somente depois
da origem válida. `STOP` ou `PAUSE` cancelam a espera; timeout bloqueia o ensaio
com `RECIP_ENCODER_NO_ORIGIN` e não é contabilizado como perda de aquisição da
correção dinâmica.

## Validação

```powershell
python -m unittest discover -s "Supervisório/tests" -p "test_*.py"
EncoderCore\build_vs2022\Release\encoder_state_tests.exe
EncoderCore\build_vs2022\Release\encoder_control_protocol_tests.exe
EncoderCore\build_vs2022\Release\recip_encoder_controller_tests.exe
DriveA5\build_vs2022\Release\a5_speed_logger.exe --self-test
DriveA5\build_vs2022\Release\a5_speed_logger.exe --self-test-encoder-link
```

O contínuo e o reciprocante foram validados em bancada com o pipeline principal
a 200 Hz; o reciprocante operacional foi exercitado de 1 a 3 mm/s. A latência
e o erro de extremo ainda devem ser acompanhados durante a ampliação gradual
para 5, 10, 15 e 20 mm/s.
