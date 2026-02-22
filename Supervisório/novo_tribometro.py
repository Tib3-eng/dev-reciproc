import os
import time
import threading
import subprocess
import tempfile
import tkinter
from tkinter import messagebox
from tkinter import ttk

import numpy as np
from matplotlib.animation import FuncAnimation
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.figure import Figure

# Modulos do hardware DLG/LDTP (podem nao existir no ambiente do usuario final)
# Quando ausentes, o modo externo (DLG+Drive em C) continua funcionando.
try:
    import LDTP_Const
    import myModule
    HAVE_LDTP = True
    _LDTP_IMPORT_ERROR = ""
except Exception as e:
    HAVE_LDTP = False
    _LDTP_IMPORT_ERROR = str(e)

    class _LDTPConst:
        LDTP_CHTYPE_ANIN = 0
        LDTP_CHTYPE_DIGIN = 1

    LDTP_Const = _LDTPConst()

    class _MyModuleStub:
        def SetAnOutV(self, *args, **kwargs):
            return None
        def SetDigOut(self, *args, **kwargs):
            return None

    myModule = _MyModuleStub()

import orchestrator_runtime as orch

# Execucao dos loggers C em background (DLG + Drive + merge)
USE_EXTERNAL_RUNNER = True
external_run_state = None

# Pasta base padrao para salvar ensaios
REPO_BASE = os.path.join(os.path.expanduser("~"), "Desktop", "Repositorio")

# Log simples para a aba de debug (quando existir)
def log_msg(msg):
    ts = time.strftime('%H:%M:%S')
    line = f"[{ts}] {msg}"
    print(line)


    if 'log_text' not in globals():
        return

    def append():
        try:
            log_text.configure(state='normal')
            log_text.insert('end', line + "\n")
            log_text.see('end')
            log_text.configure(state='disabled')
        except Exception:
            pass

    try:
        root.after(0, append)
    except Exception:
        # Se root nao existir por algum motivo, ignore.
        pass


# Aviso unico se o modulo LDTP nao estiver disponivel.
if not HAVE_LDTP:
    log_msg(f"Aviso: LDTP_Const/myModule nao encontrados; controle analogico interno desativado ({_LDTP_IMPORT_ERROR}).")

def _set_status(label, ok):
    if label is None:
        return
    if ok:
        label.config(text='OK', fg='green')
    else:
        label.config(text='X', fg='red')


# Check simples de comunicacao (DLG + Drive)
def check_status():
    log_msg('Check status: iniciando.')

    dlg_ok = False
    drive_ok = False

    repo_root = orch.find_repo_root()

    # ---- DLG ----
    try:
        exe_info = orch.check_executables(repo_root)
        dlg_exe = exe_info.get('dlg_exe')
        if not dlg_exe:
            log_msg('DLG check: dlg_logger_ipc.exe nao encontrado.')
        else:
            log_msg('DLG check: executando dlg_logger_ipc (1s)...')
            with tempfile.NamedTemporaryFile(delete=False, suffix='.csv') as tmp:
                tmp_path = tmp.name
            dlg_ip = getattr(orch, 'DEFAULT_DLG_IP', '192.168.1.100')
            dlg_port = getattr(orch, 'DEFAULT_DLG_PORT', 41401)
            cmd = [
                dlg_exe,
                '--out', tmp_path,
                '--duration', '1',
                '--rate', f"{getattr(orch, 'DEFAULT_RATE_HZ', 50.0):.0f}",
                '--ip', str(dlg_ip),
                '--port', str(dlg_port),
                '--bind-port', '0',
            ]
            try:
                subprocess.run(cmd, capture_output=True, text=True, timeout=5)
            except subprocess.TimeoutExpired:
                log_msg('DLG check: timeout executando logger.')

            try:
                with open(tmp_path, 'r', encoding='utf-8') as f:
                    # pula cabecalho
                    next(f, None)
                    for line in f:
                        line = line.strip()
                        if not line:
                            continue
                        # err e a ultima coluna
                        parts = line.split(',')
                        if parts and parts[-1].strip() == '0':
                            dlg_ok = True
                            break
            except Exception:
                pass
            try:
                os.remove(tmp_path)
            except Exception:
                pass

            if dlg_ok:
                log_msg('DLG check: ok.')
            else:
                log_msg('DLG check: nenhuma amostra valida.')
    except Exception as e:
        log_msg(f'DLG check: erro ({e}).')

    # ---- Drive ----
    try:
        # Preferir a5_pos_cli (diag) para nao mexer em parametros
        candidates = [
            os.path.join(repo_root, 'DriveA5', 'build', 'Release', 'a5_pos_cli.exe'),
            os.path.join(repo_root, 'DriveA5', 'build', 'a5_pos_cli.exe'),
        ]
        drive_exe = None
        for c in candidates:
            if os.path.exists(c):
                drive_exe = c
                break

        if not drive_exe:
            log_msg('Drive check: a5_pos_cli.exe nao encontrado.')
        else:
            # COM4 e padrao do projeto
            cmd = [drive_exe, 'COM4', '--diag']
            res = subprocess.run(cmd, capture_output=True, text=True, timeout=5)
            drive_ok = (res.returncode == 0)
            if drive_ok:
                log_msg('Drive check: ok.')
            else:
                log_msg('Drive check: falhou (diag).')
    except Exception as e:
        log_msg(f'Drive check: erro ({e}).')

    try:
        _set_status(status_dlg_value, dlg_ok)
        _set_status(status_drive_value, drive_ok)
    except Exception:
        pass

# INICIALIZAÇÃO DE VARIÁVEIS PADRÃO
aux = 2000
freq = 50.0
running = False
ip = '127.0.0.1'
channels = []
for _ in range(8): channels.append([ip, LDTP_Const.LDTP_CHTYPE_ANIN, 0])
channels.append([ip, LDTP_Const.LDTP_CHTYPE_DIGIN, 0])
numChannels = len(channels)

caminho_arquivo_1 = ""
caminho_arquivo_2 = ""
# Caminhos usados pelo pipeline externo (DLG/Drive/Merge)
caminho_dlg_csv = ""
caminho_drive_csv = ""
caminho_merge_csv = ""
caminho_schedule_csv = ""

# Inicializa listas de amostras

lista_entries_velocidade = []
lista_entries_distancia = []
lista_labels_voltas_cursos = []
lista_labels_duracao = []
# allSamps agora servirá apenas como BUFFER temporário para salvar no disco
allSamps = [[] for _ in range(numChannels)] 
graSamps = [[] for _ in range(numChannels)]
sampsTimestamp = []
tensao_vel = []
duracao = []
soma_tempos_vel = []
start_time = 0
timer_started = False

# Controle de escala Y (auto por padrao).
# Mantemos valores padrao para evitar NameError mesmo sem UI dedicada.
y1_auto = True
y2_auto = True
y1_min = 0.0
y1_max = 1.0
y2_min = 0.0
y2_max = 1.0

# Dados do grafico 3 (atrito). Inicializa para evitar NameError.
p_strokes = []
p_atrito_ef = []
p_atrito_max = []
p_atrito_min = []
p_coluna_velocidade = []

# Configuração do tamanho do bloco para salvar no disco (ex: a cada 1000 linhas)
tamanho_bloco = 1000 


# Nicolas
'''
É responsável por simular a aquisição de dados em tempo real dos gráficos da Temperatura e Força.
Ela abre o arquivo _T.txt. Lê uma linha, processa e dá um time.sleep(1.0/200.0) para simular que a aquisição está ocorrendo a 200 amostras por segundo.

Ela acumula os dados no buffer_escrita e, a cada 1000 linhas, salva no arquivo final. Isso evita desgastar o disco.
Sweep: É esta função que verifica se o gráfico encheu (if len(graSamps[0]) >= aux). Se encheu, ela limpa a lista para o gráfico começar a desenhar de novo.

'''
def go():
    global running, aux, freq, start_time, contador_amostras_total
    global graSamps, allSamps, sampsTimestamp
    global caminho_arquivo_1, caminho_arquivo_2

    running = "true"
    try:
        label_ensaio_estado.config(text="Em andamento")
    except Exception:
        pass

    # Nome do arquivo de leitura (entrada simulada)
    nome_arquivo_leitura = "2025.12.12-VERM_28-1_T.txt"
    print(f"Iniciando leitura (STREAMING) de: {nome_arquivo_leitura}")

    # Verifica se o arquivo de leitura existe no computador
    if not os.path.exists(nome_arquivo_leitura):
        print("ERRO: Arquivo de entrada não encontrado.")
        running = False
        return

    try:
        # Abre o arquivo de LEITURA em modo read
        with open(nome_arquivo_leitura, 'r') as f_in:
            
            # --- Pular cabeçalho ---
            # Lê linha a linha até achar dados, sem carregar tudo na RAM
            last_pos = 0
            while True:
                last_pos = f_in.tell() # Marca posição atual
                line = f_in.readline()
                if not line: 
                    break
                
                clean_line = line.strip()
                # Verifica se é dado numérico
                if len(clean_line) > 0 and (clean_line[0].isdigit() or clean_line[0] == '-') and ';' in clean_line:
                    f_in.seek(last_pos) # Volta para o início da linha de dados válida
                    break
            
            print("iniciando aquisição...")

            # Buffer temporário para escrita em disco
            buffer_escrita = [] 

            # Loop de leitura linha por linha (Streaming)
            while running == "true":

                if is_paused:
                    time.sleep(0.1) # Para um pouco para não travar a CPU
                    continue

                line = f_in.readline()
                
                # Se acabou o arquivo de entrada, para
                if not line: 
                    break

                parts = line.strip().split(';')

                # Verifica se a linha tem colunas suficientes (Canais + Tempo)
                if len(parts) >= numChannels + 1:
                    try:
                        vals = [float(p.replace(',', '.')) for p in parts]

                        contador_amostras_total += 1

                        # 1. LÓGICA DO GRÁFICO (SWEEP)
                        reset_grafico = False
                        if len(graSamps[0]) >= aux:
                            reset_grafico = True
                            sampsTimestamp.clear() # Limpa o tempo junto com os dados

                        # Calcula quantos segundos cabem na tela (ex: 2000 amostras / 200Hz = 10s)
                        limite_tempo_s = aux / freq
                        # Captura o tempo real atual (sincronizado com cronômetro)
                        tempo_agora = time.time() - start_time

                        # Verifica quanto tempo esse gráfico atual já durou
                        # Se já houver dados no gráfico, calcula quanto tempo passou desde o primeiro ponto da tela atual
                        tempo_relativo = 0
                        if len(sampsTimestamp) > 0:
                            tempo_relativo = tempo_agora - sampsTimestamp[0]

                        #Verifica as duas condições de estouro
                        estourou_amostras = (len(graSamps[0]) >= aux)
                        estourou_tempo = (tempo_relativo >= limite_tempo_s)

                        # Se qualquer um dos dois acontecer, limpa a tela
                        reset_grafico = False
                        if estourou_amostras or estourou_tempo:
                            reset_grafico = True
                            sampsTimestamp.clear()

                        sampsTimestamp.append(tempo_agora)

                        # 2. PROCESSAMENTO DOS CANAIS
                        # Monta uma string para salvar no arquivo de saída txt
                        ## (primeiro valor é o tempo/coluna 0)
                        linha_para_salvar = f"{vals[0]:.3f}" # Timestamp
                        
                        # Loop para percorrer cada canal de sensor (0 a 8)
                        for iCh in range(numChannels):
                            val = vals[iCh + 1] # Pega o valor na lista de valores convertidos
                            
                            # Adiciona ao gráfico
                            if reset_grafico:
                                graSamps[iCh].clear()
                            graSamps[iCh].append(val) # Adiciona o novo valor na lista de plotagem do gráfico
                            
                            linha_para_salvar += f"; {val:.3f}" # Adiciona string do valor

                        # Adiciona a linha no buffer da memória
                        buffer_escrita.append(linha_para_salvar + "\n")

                        # --- 3. ESCRITA NO DISCO (BUFFER) ---
                        # Se o buffer encheu, DESCARREGA no disco e limpa a RAM
                        if len(buffer_escrita) >= tamanho_bloco:
                            if caminho_arquivo_2 != "":
                                with open(caminho_arquivo_2, 'a') as f_out: # Modo 'a' (append)
                                    # Escreve todas as linhas do buffer de uma vez no disco
                                    f_out.writelines(buffer_escrita)
                            # Limpa o buffer da memória RAM para recomeçar a acumular
                            buffer_escrita.clear() 

                    except ValueError:
                        pass

                # Simula a taxa de amostragem
                # Pausa a execução por 0.005s (1/200) para simular a frequência de 200Hz
                time.sleep(1.0 / freq)

            # --- FINALIZAÇÃO ---
            # Se sobrou algo no buffer ao parar, salva o resto
            if len(buffer_escrita) > 0 and caminho_arquivo_2 != "":
                 with open(caminho_arquivo_2, 'a') as f_out:
                    f_out.writelines(buffer_escrita)
            
            print("Aquisição finalizada.")
            running = False

    except Exception as e:
        print(f"Erro crítico na thread go: {e}")
        running = False


# Nicolas
################# FUNÇÃO GO PARA O TERCEIRO GRÁFICO (ARQUIVO _P)
'''
# Ela abre o arquivo _P e lê linha por linha
# O Gráfico 3 não é de tempo contínuo, é ponto a ponto (stroke). A função lê o tempo em que aquele stroke aconteceu e fica em pausa (while) esperando o relógio principal (função go) chegar naquele tempo.

#Exemplo: O stroke 5 aconteceu no segundo 10. A função go_p lê isso e espera a simulação chegar no segundo 10. Quando chega, ela libera os dados.
'''

def go_p():
    """Lê o arquivo processado (_P.txt) para o Gráfico 3 de Atrito"""
    global running, p_strokes, p_atrito_ef, p_atrito_max, p_atrito_min, p_coluna_velocidade
    global graSamps, freq, contador_amostras_total, sampsTimestamp
    
    nome_arquivo_p = "2025.12.12-VERM_28-1_P.txt"
    
    if not os.path.exists(nome_arquivo_p):
        print(f"ERRO: Arquivo {nome_arquivo_p} não encontrado.")
        return

    try:
        with open(nome_arquivo_p, 'r') as f_p:
            # Pula o cabeçalho (Linha 1)
            f_p.readline() 
            
            while running == "true":
            
                line = f_p.readline()
                if not line: 
                    break
                
                # Separa as colunas pelo TAB
                parts = line.strip().split('\t')
                
                if len(parts) >= 10:

                    try:
                        # Coluna 0 do arquivo é o TEMPO em minutos
                        tempo_stroke_min = float(parts[0].replace(',', '.'))
                        tempo_stroke_seg = tempo_stroke_min * 60.0
                        
                        # --- SINCRONIZAÇÃO TEMPO REAL - Espera o relógio principal ---
                        # Este while trava esta função aqui até que a função go() chegue no tempo certo
                        while True:
                            if running != "true": 
                                break
                            
                            # Pega o último tempo registrado pela função go()
                            if len(sampsTimestamp) > 0:
                                tempo_atual_simulacao = sampsTimestamp[-1]
                            else:
                                tempo_atual_simulacao = 0
                        
                            # Compara se o tempo da simulação já passou ou igualou o tempo desse dado
                            if tempo_atual_simulacao >= tempo_stroke_seg:
                                break # Plotar
                            else:
                                time.sleep(0.005) # Espera um pouco

                        if running != "true": 
                            break

                        # EXTRAÇÃO DOS DADOS PARA O GRÁFICO
                        strk = float(parts[1].replace(',', '.'))
                        ef = float(parts[2].replace(',', '.'))
                        mx = float(parts[4].replace(',', '.'))
                        mn = float(parts[6].replace(',', '.'))
                        vel_3graf = float(parts[9].replace(',', '.'))
                        
                        # Adiciona os valores nas listas globais para a função update3 desenhar
                        p_strokes.append(strk)
                        p_atrito_ef.append(ef)
                        p_atrito_max.append(mx)
                        p_atrito_min.append(mn)
                        p_coluna_velocidade.append(vel_3graf)
                        
                    except ValueError: pass
                
                
    except Exception as e:
        print(f"Erro na thread go_p: {e}")

##########################################################################################3


def fechar_janela():
    myModule.SetDigOut(ip, 0, 1, 0)
    root.quit()


def salvar_arquivo():
    
    global caminho_arquivo_1, caminho_arquivo_2, caminho_pasta
    global caminho_dlg_csv, caminho_drive_csv, caminho_merge_csv, caminho_schedule_csv

    # ---------------------------------------------------------------------------------
    # MUDANCA CONSCIENTE: removemos a caixa de dialogo e padronizamos a pasta.
    # Pasta base: Desktop\\Repositorio
    # Subpasta: "Nome do ensaio - Estudo"
    # Se a subpasta ja existir, bloqueamos o inicio do ensaio.
    # ---------------------------------------------------------------------------------
    nome_ensaio = entries_left["Nome do ensaio"].get().strip()
    estudo = entries_left["Estudo"].get().strip()

    # Normaliza nome da pasta para Windows (sem caracteres proibidos)
    nome_pasta = orch.sanitize_folder_name(f"{nome_ensaio} - {estudo}")
    caminho_pasta = os.path.join(REPO_BASE, nome_pasta)

    # Garante pasta base
    if not os.path.exists(REPO_BASE):
        os.makedirs(REPO_BASE, exist_ok=True)

    # Bloqueia se ja existe
    if os.path.exists(caminho_pasta):
        messagebox.showwarning(
            "Nome já existe",
            "Já existe um ensaio com este nome.\n\n"
            "Altere 'Nome do ensaio' ou 'Estudo' para continuar."
        )
        caminho_arquivo_1 = ""
        caminho_arquivo_2 = ""
        return

    os.makedirs(caminho_pasta, exist_ok=True)

    # Nomes padrao dos arquivos dentro da subpasta
    caminho_arquivo_1 = os.path.join(caminho_pasta, "info.csv")   # metadados do ensaio
    caminho_arquivo_2 = os.path.join(caminho_pasta, "dlg.csv")    # mantido para compatibilidade
    caminho_dlg_csv = os.path.join(caminho_pasta, "dlg.csv")
    caminho_drive_csv = os.path.join(caminho_pasta, "drive.csv")
    caminho_merge_csv = os.path.join(caminho_pasta, "merge.csv")
    caminho_schedule_csv = os.path.join(caminho_pasta, "schedule.csv")

    # Cria arquivos vazios (evita erros de permissao na hora do append)
    for p in [caminho_arquivo_1, caminho_arquivo_2]:
        with open(p, "w", encoding="utf-8") as f:
            f.write("")
   


def converte(): 
    pass

    '''
    # Caminho do programa e parâmetros
    programa1 = r'conversor.exe'

    # trocar por eventuaus parametros que o programa possa vir a precisar
    """
    parametro1_programa2 = str(freq)
    parametro2_programa2 = str(timer)
    parametro3_programa2 = str(ip)
    parametro4_programa2 = str(filePath)
    parametro5_programa2 = str(caminho_arquivo_1)
    """

    # Criando a lista de argumentos (incluindo o programa e seus parâmetros)
    comando1 = [programa1]

    # Chamando o programa e capturando a saída
    resultado = subprocess.run(comando1, capture_output=True, text=True)

    # Imprimir a saída do programa chamado
    print('Saída do programa:')
    print(resultado.stdout)

    # Se ocorrer algum erro, imprima a mensagem de erro
    if resultado.stderr:
        print('Erro:')
        print(resultado.stderr)
    '''


def gera_grafico():
    pass
    '''
    time.sleep(360)
    # Caminho do programa e parâmetros
    programa2 = r'grafico3_9.exe'

    # trocar por eventuaus parametros que o programa possa vir a precisar
    """
    parametro1_programa2 = str(freq)
    parametro2_programa2 = str(timer)
    parametro3_programa2 = str(ip)
    parametro4_programa2 = str(filePath)
    parametro5_programa2 = str(caminho_arquivo_1)
    """

    # Criando a lista de argumentos (incluindo o programa e seus parâmetros)
    comando2 = [programa2]

    # Chamando o programa e capturando a saída
    resultado = subprocess.run(comando2, capture_output=True, text=True)

    # Imprimir a saída do programa chamado
    print('Saída do programa:')
    print(resultado.stdout)

    # Se ocorrer algum erro, imprima a mensagem de erro
    if resultado.stderr:
        print('Erro:')
        print(resultado.stderr)
    '''

def velocidades():
    if not HAVE_LDTP:
        log_msg('LDTP nao disponivel; controle analogico interno desativado.')
        return
    
    # Pega as listas que criamos no start_acquisition
    global lista_velocidades_digitadas, lista_duracao
    global ip, running, is_paused
    
    # Se você precisar converter mm/s para Volts para a máquina:
    fator_conversao = 1.0 

    for i in range(len(lista_velocidades_digitadas)):
        
        # Se o usuário mandou Parar o ensaio, quebra o loop imediatamente
        if running != "true":
            break
            
        # 1. Pega os dados dessa etapa específica
        vel_atual_mms = lista_velocidades_digitadas[i]  # Valor para mostrar (mm/s)
        duracao_atual_s = lista_duracao[i]              # Duração dessa etapa (segundos)

        # 2. Atualiza os labels de alvo atual (velocidade linear e RPM alvo)
        try:
            raio_mm = float(ent_raio.get().strip().replace(",", "."))
        except Exception:
            raio_mm = 0.0
        rpm_alvo = orch.rpm_from_mm_s(vel_atual_mms, raio_mm) if raio_mm > 0 else 0
        _set_target_labels(f"{vel_atual_mms} mm/s", f"{rpm_alvo} rpm")

        # 3. Manda o comando para a máquina
        # (Se precisar converter para Volts)
        tensao_para_enviar = vel_atual_mms * fator_conversao
        
        myModule.SetAnOutV(ip, tensao_para_enviar, 0, 100)
        # myModule.SetAnOutV(ip, tensao_para_enviar, 0, 100)

        tempo_rest = duracao_atual_s
        motor_estava_parado = False

        while tempo_rest > 0:
            if running != "true":
                break

            if is_paused:
                # 1. Se acabou de entrar em pausa, para o motor
                if not motor_estava_parado:
                    myModule.SetAnOutV(ip, 0, 0, 100) # Zera a tensão
                    motor_estava_parado = True
                    # O label pode indicar que está pausado
                    _set_target_labels(f"{vel_atual_mms} mm/s (Pausado)", f"{rpm_alvo} rpm")
                
                # 2. Espera sem descontar o tempo (congela o cronômetro da etapa)
                time.sleep(0.1)
                continue 

            # SE ESTIVER RODANDO (NÃO PAUSADO):
            else:
                # Se estava parado antes, religa o motor na velocidade certa
                if motor_estava_parado:
                    myModule.SetAnOutV(ip, tensao_para_enviar, 0, 100)
                    _set_target_labels(f"{vel_atual_mms} mm/s", f"{rpm_alvo} rpm")
                    motor_estava_parado = False

                # Desconta o tempo
                passo = 0.1
                if passo > tempo_rest: 
                    passo = tempo_rest
                
                time.sleep(passo)
                tempo_rest -= passo

        if running != "true":
            break

    # Quando terminar tudo, mostra "Fim" ou zera
    if running == "true":
        try:
            label_ensaio_estado.config(text="Finalizado")
            _set_targets_stopped()
        except Exception:
            pass
        myModule.SetAnOutV(ip, 0, 0, 100) # Para a máquina

        running = False 
        is_paused = False
        
        # Reseta o visual do botão Pausar
        if 'button_frame5_pausar' in globals():
            button_frame5_pausar.config(text="Pausar", bg="SystemButtonFace")



def _format_hms(seconds):
    try:
        seconds = int(seconds)
    except Exception:
        seconds = 0
    if seconds < 0:
        seconds = 0
    h = seconds // 3600
    m = (seconds % 3600) // 60
    s = seconds % 60
    return f"{h}:{m:02d}:{s:02d}"

def atualiza_decorrido():
    """Atualiza o cronometro (tempo restante se houver duracao total)."""
    # Aceita tanto bool quanto string "true" para evitar travar o timer.
    if not running:
        return

    try:
        total_s = sum(lista_duracao) if "lista_duracao" in globals() and lista_duracao else 0
    except Exception:
        total_s = 0

    try:
        elapsed = time.time() - start_time
    except Exception:
        elapsed = 0

    if total_s > 0:
        remaining = total_s - elapsed
        if remaining < 0:
            remaining = 0
        display = _format_hms(remaining)
    else:
        display = _format_hms(elapsed)

    try:
        lbl_tempo_decorrido2.config(text=display)
    except Exception:
        pass

    try:
        root.after(200, atualiza_decorrido)
    except Exception:
        pass

def _start_timer_now():
    """Inicia o cronometro no instante atual (thread-safe via root.after)."""
    global start_time, timer_started
    start_time = time.time()
    timer_started = True
    atualiza_decorrido()

def _is_running():
    return (running == "true") or (running is True)

def _set_target_labels(vel_txt=None, rpm_txt=None):
    def _do():
        if vel_txt is not None:
            try:
                label_ensaio_vel.config(text=vel_txt)
            except Exception:
                pass
        if rpm_txt is not None:
            try:
                label_ensaio_rpm.config(text=rpm_txt)
            except Exception:
                pass

    try:
        root.after(0, _do)
    except Exception:
        _do()

def _set_targets_stopped():
    _set_target_labels("Parado", "0 rpm")

def _update_vel_label_from_schedule(vel_mm_s_list, dur_s_list, rpm_list=None):
    """
    Atualiza o label de velocidade baseado no schedule (modo externo).
    """
    # Aguarda o inicio efetivo do tempo
    while _is_running() and not timer_started:
        time.sleep(0.05)

    total = 0.0
    for d in dur_s_list:
        try:
            total += float(d)
        except Exception:
            pass

    while _is_running():
        try:
            elapsed = time.time() - start_time
        except Exception:
            elapsed = 0

        if elapsed >= total:
            _set_targets_stopped()
            break

        # Descobre o segmento atual
        acc = 0.0
        idx = 0
        for i, d in enumerate(dur_s_list):
            acc += float(d)
            if elapsed <= acc:
                idx = i
                break

        try:
            v = float(vel_mm_s_list[idx])
            v_txt = f"{v:.2f} mm/s"
        except Exception:
            v_txt = None

        rpm_txt = None
        try:
            if rpm_list is not None and idx < len(rpm_list):
                rpm_txt = f"{int(rpm_list[idx])} rpm"
        except Exception:
            rpm_txt = None

        _set_target_labels(v_txt, rpm_txt)

        time.sleep(0.2)
def _wait_dlg_ok_and_start_timer(dlg_csv_path, min_ok=3, timeout_s=15):
    """
    Aguarda N amostras validas do DLG (err=0) e so entao inicia o cronometro.
    Isso alinha o tempo do GUI com o inicio real do DLG.
    """
    log_msg(f"Aguardando {min_ok} amostras validas do DLG para iniciar tempo...")
    t0 = time.time()
    ok_count = 0

    # Espera o arquivo existir
    while _is_running() and not os.path.exists(dlg_csv_path):
        if time.time() - t0 > timeout_s:
            log_msg("DLG: timeout aguardando arquivo CSV.")
            return
        time.sleep(0.05)

    try:
        with open(dlg_csv_path, "r", encoding="utf-8") as f:
            # Pula cabecalho se existir
            first = f.readline()
            if first and "idx" not in first:
                # Linha nao era cabecalho; processa como dado
                f.seek(0)

            while _is_running():
                line = f.readline()
                if not line:
                    if time.time() - t0 > timeout_s:
                        log_msg("DLG: timeout aguardando amostras validas.")
                        return
                    time.sleep(0.05)
                    continue

                parts = line.strip().split(",")
                if not parts:
                    continue

                # DLG CSV: ... , err (ultima coluna)
                if parts[-1].strip() == "0":
                    ok_count += 1
                    if ok_count >= min_ok:
                        try:
                            root.after(0, _start_timer_now)
                        except Exception:
                            _start_timer_now()
                        log_msg("DLG: amostras validas confirmadas; tempo iniciado.")
                        return
    except Exception as e:
        log_msg(f"DLG: erro aguardando amostras ({e}).")

def _tail_dlg_csv_for_graphs(dlg_csv_path):
    """
    Alimenta os graficos com dados do DLG (modo externo).
    Lê dlg.csv em tempo real e atualiza graSamps + sampsTimestamp.
    """
    # Espera o arquivo existir
    t0 = time.time()
    while _is_running() and not os.path.exists(dlg_csv_path):
        if time.time() - t0 > 15:
            log_msg("DLG: timeout aguardando dlg.csv para graficos.")
            return
        time.sleep(0.05)

    try:
        with open(dlg_csv_path, "r", encoding="utf-8") as f:
            # Pula cabecalho
            header = f.readline()
            if header and "idx" not in header:
                f.seek(0)

            while _is_running():
                line = f.readline()
                if not line:
                    time.sleep(0.02)
                    continue

                parts = line.strip().split(",")
                # Esperado: idx,t_qpc,t_s,ch1..ch8,err
                if len(parts) < 12:
                    continue

                try:
                    t_s = float(parts[2])
                except Exception:
                    continue

                err = parts[-1].strip()
                # Para err=1 ou NULL, coloca NaN nos canais (mantem tempo).
                vals = []
                for i in range(8):
                    v = parts[3 + i].strip()
                    if v == "NULL" or err != "0":
                        vals.append(float("nan"))
                    else:
                        try:
                            vals.append(float(v))
                        except Exception:
                            vals.append(float("nan"))

                sampsTimestamp.append(t_s)
                for i in range(8):
                    graSamps[i].append(vals[i])

                # Janela deslizante para evitar crescer indefinidamente
                if len(sampsTimestamp) > aux:
                    del sampsTimestamp[:-aux]
                    for i in range(8):
                        del graSamps[i][:-aux]
    except Exception as e:
        log_msg(f"DLG: erro lendo dlg.csv para graficos ({e}).")
def start_acquisition():

    global running
    # NOTE: declare globals before any assignment inside this function to avoid
    # Python "assigned before global declaration" errors (PyInstaller parse).
    global start_time, is_paused

    # Impede o funcionamento do botão iniciar duas vezes seguidas:
    if running == "true":
        messagebox.showwarning("Atenção", 
                               "O ensaio já está rodando!\n\n"
                               "Para iniciar um novo, clique em 'Parar'")
        return

    ###################  VALIDAÇÕES PARA QUE O PROGRAMA INICIE:
    ## Primeira tabela obrigatória (Superior esquerda)
    for nome_campo, widget_entry in entries_left.items():
        valor = widget_entry.get()
        
        # .strip() remove espaços em branco antes e depois. 
        # Se o usuário digitar só " ", o sistema considera vazio.
        if not valor.strip():
            messagebox.showwarning("Campo Obrigatório", f"O ensaio não pode iniciar.\n\nPor favor, preencha o campo: '{nome_campo}'")
            return
        
        if nome_campo == "Estudo":
            try:
                int(valor) # Tenta transformar o texto em número inteiro
            except ValueError:
                messagebox.showwarning("Valor Inválido", 
                                       f"O campo '{nome_campo}' deve ser um número inteiro")
                return
            
        if nome_campo == "Repetição":
            try:
                int(valor) # Tenta transformar o texto em número inteiro
            except ValueError:
                messagebox.showwarning("Valor Inválido", 
                                    f"O campo '{nome_campo}' deve ser um número inteiro")
                return
            
        if nome_campo == "Diâmetro da esfera [mm]":
            try:
                float(valor.replace(',', '.'))
            except ValueError:
                messagebox.showwarning("Valor Inválido", 
                                    f"O campo '{nome_campo}' deve ser um número.")
                return
            
        if nome_campo == "Corpo de prova (bloco)":
            try:
                int(valor) # Tenta transformar o texto em número inteiro
            except ValueError:
                messagebox.showwarning("Valor Inválido", 
                                    f"O campo '{nome_campo}' deve ser um número inteiro")
                return

    ## Segunda tabela - lubrificado (Superior direita)
    if lubrificado_var.get():
        minimo_um_completo = False

        # Percorre as 5 linhas
        for i in range(5):
            mat_text = lista_entries_material[i].get().strip()
            pct_text = lista_entries_porcentagem[i].get().strip()

            # CASO 1: Linha Completamente Vazia (Ignora e vai pra próxima)
            if not mat_text and not pct_text:
                continue

            # CASO 2: Linha Cheia (Marca que achamos dados válidos)
            elif mat_text and pct_text:
                try:
                    float(pct_text.replace(',', '.'))
                    minimo_um_completo = True
                except ValueError:
                    messagebox.showwarning("Erro!",
                                        f"Valor Inválido na linha {i+1} da tabela de Lubrificação\n\n"
                                        "O campo 'Porcentagem' deve ser um número.")
                    return

            # CASO 3: Só Material preenchido (ERRO!)
            elif mat_text and not pct_text:
                messagebox.showwarning("Dados Incompletos", 
                                       f"Erro na linha {i+1} da tabela de Lubrificação.\n\n"
                                       "Você preencheu o Material, mas esqueceu a %.")
                return

            # CASO 4: Só Porcentagem preenchida (ERRO!)
            elif pct_text and not mat_text:
                messagebox.showwarning("Dados Incompletos", 
                                       f"Erro na linha {i+1} da tabela de Lubrificação.\n\n"
                                       "Você preencheu a %, mas esqueceu o Material.")
                return

        # Se não deu erro acima, mas tudo estava vazio
        if not minimo_um_completo:
            messagebox.showwarning("Dados de Lubrificação", 
                                   "A opção 'Lubrificado' está marcada.\n\n"
                                   "Você deve preencher pelo menos um Material e sua respectiva % na mesma linha.")
            return

    ############
    ## Verificação Raio e Força normal (Meio da interface)
    valor_raio = ent_raio.get()
    nome_caixa_raio = lbl_raio['text'] 
    valor_forca = ent_forca.get() 
    nome_caixa_forca = lbl_forca['text']      
    # .strip() remove espaços em branco antes e depois. 
    # Se o usuário digitar só " ", o sistema considera vazio.
    if not valor_raio.strip():
        messagebox.showwarning("Campo Obrigatório", f"O ensaio não pode iniciar.\n\nPor favor, preencha o campo: '{nome_caixa_raio}'")
        return
    
    try:
        float(valor_raio.strip().replace(',', '.'))
    except ValueError:
            messagebox.showwarning("Valor Inválido", 
                            f"O campo '{nome_caixa_raio}' deve ser um número.")
            return

    if not valor_forca.strip():
        messagebox.showwarning("Campo Obrigatório", f"O ensaio não pode iniciar.\n\nPor favor, preencha o campo: '{nome_caixa_forca}'")
        return
    
    try:
        float(valor_forca.strip().replace(',', '.'))
    except ValueError:
        messagebox.showwarning("Valor Inválido", 
                               f"O campo '{nome_caixa_forca}' deve ser um número.")
        return


    ###############
    ## Verificação reciprocating, se o campo Curso está preenchido
    if reciprocante_var.get():
        nome_caixa_curso = lbl_curso['text'] 
        # Verifica se o campo curso está vazio (ignorando espaços em branco)
        if not entry_curso.get().strip():
            messagebox.showwarning("Campo Obrigatório", 
                                   f"A opção Reciprocating está marcada.\n\nPor favor, preencha o campo: '{nome_caixa_curso}'")
            return
        try:
            float(entry_curso.get().strip().replace(',', '.'))
        except ValueError:
            messagebox.showwarning("Valor Inválido", 
                                f"O campo '{nome_caixa_curso}' deve ser um número válido.")
            return
        
    
    ##############
    ## Verificação campos Velocidade e Distância:
    par_vel_dist_encontrado = False

    # Varre as 11 linhas da tabela
    for i in range(11):
        # Pega o texto da linha 'i' de cada lista
        vel_txt = lista_entries_velocidade[i].get().strip()
        dist_txt = lista_entries_distancia[i].get().strip()

        # CASO 1: Ambos preenchidos (Linha válida)
        if vel_txt and dist_txt:
            try:
                float(vel_txt.replace(',', '.'))
        
                try:
                    float(dist_txt.replace(',', '.'))
                    par_vel_dist_encontrado = True

                    '''
                    if vel_num > 0:
                        duracao_tempo_minutos = dist_num / vel_num / 60.0


                        lista_labels_duracao[i].config(text=f"{duracao_tempo_minutos}")

                    else:
                        lista_labels_duracao[i].config(text="Erro")

                    '''
                    
                except ValueError:
                    messagebox.showwarning("Erro!",
                                        f"Valor Inválido na linha {i+1} da tabela das Distâncias\n\n"
                                        "O campo 'Distâncias' deve ser um número.")
                    return

                
            except ValueError:
                messagebox.showwarning("Erro!",
                                        f"Valor Inválido na linha {i+1} da tabela da Velocidade\n\n"
                                        "O campo 'Velocidade' deve ser um número.")
                return

        
        # CASO 2: Apenas Velocidade preenchida (Erro de inconsistência)
        elif vel_txt and not dist_txt:
            messagebox.showwarning("Erro!", 
                                   f"Dados incompletos na linha {i+1} da tabela inferior:\n"
                                   "Você preencheu a Velocidade, mas esqueceu a Distância.")
            return

        # CASO 3: Apenas Distância preenchida (Erro de inconsistência)
        elif dist_txt and not vel_txt:
            messagebox.showwarning("Erro!", 
                                   f"Dados incompletos na linha {i+1} da tabela inferior:\n"
                                   "Você preencheu a Distância, mas esqueceu a Velocidade.")
            return

    # CASO 4: Varreu tudo e não achou nenhum par completo
    if not par_vel_dist_encontrado:
        messagebox.showwarning("Tabela Vazia", 
                               "É necessário preencher pelo menos uma etapa do ensaio.\n\n"
                               "Preencha uma Velocidade e uma Distância na mesma linha.")
        return
        

### FIM DAS VALIDAÇÕES
###########################################################################################################################
    ####### INÍCIO DO PROGRAMA - GERAÇÃO DA PASTA E ARQUIVOS TXT

    # Reseta a variável antes de chamar (garante que não pegue lixo de memória)
    global caminho_arquivo_1, caminho_arquivo_2
    caminho_arquivo_1 = ""
    caminho_arquivo_2 = "" 
    global qte_vel, lista_velocidades_digitadas, lista_duracao

    # Pre-check executaveis C antes de criar pasta/arquivos
    repo_root = orch.find_repo_root()
    exe_info = orch.check_executables(repo_root)
    if exe_info["missing"]:
        messagebox.showerror(
            "Erro ao iniciar",
            "Falha ao iniciar loggers C.\n\nExecutaveis nao encontrados:\n- "
            + "\n- ".join(exe_info["missing"])
        )
        return

    salvar_arquivo()

    # Se o usuário clicou em "Cancelar" na janela de salvar, a variável continuará vazia.
    # Se estiver vazia, damos um 'return' para CANCELAR o início da aquisição.
    if not caminho_arquivo_1 or not caminho_arquivo_2:
        print("Início cancelado: Nenhum local escolhido para salvar.")
        return
    
    try:
        # -----------------------------------------------------------------------------
        # MUDANCA CONSCIENTE: metadados agora em CSV (info.csv) com 2-3 colunas.
        # Formato:
        #   campo,valor,valor2
        # -----------------------------------------------------------------------------
        with open(caminho_arquivo_1, 'w', encoding='utf-8') as f:
            f.write("campo,valor,valor2\n")

            # Tabela da esquerda (dados principais)
            for nome_campo, widget_entry in entries_left.items():
                valor = widget_entry.get().strip()
                if nome_campo == "Diâmetro da esfera [mm]":
                    valor = float(valor.replace(',', '.'))
                f.write(f"{nome_campo},{valor},\n")

            # Lubrificação
            texto_lub = 'sim' if lubrificado_var.get() else 'não'
            f.write(f"Lubrificado,{texto_lub},\n")
            for i in range(5):
                mat_text = lista_entries_material[i].get().strip() or "vazio"
                pct_text = lista_entries_porcentagem[i].get().strip()
                if not pct_text:
                    coluna_porcentagem = "0.00"
                else:
                    coluna_porcentagem = f"{(float(pct_text.replace(',','.'))/100):.2f}"
                f.write(f"Lubrificante {i+1},{mat_text},{coluna_porcentagem}\n")

            # Raio e Forca
            coluna_raio = float(ent_raio.get().strip().replace(',','.'))
            f.write(f"Raio [mm],{coluna_raio},\n")
            coluna_forca = float(ent_forca.get().strip().replace(',','.'))
            f.write(f"Força normal [N],{coluna_forca},\n")

            # Reciprocating e Curso
            f.write(f"Reciprocating,{'sim' if reciprocante_var.get() else 'não'},\n")
            if not entry_curso.get().strip() or not reciprocante_var:
                f.write(f"Curso [mm],0.00,\n")
            else:
                coluna_curso = float(entry_curso.get().strip().replace(',','.'))
                f.write(f"Curso [mm],{coluna_curso},\n")

            # Velocidades e Distâncias
            for i in range(11):
                if not lista_entries_velocidade[i].get().strip():
                    f.write(f"Velocidade {i+1},0.00,0.00\n")
                else:
                    val_velocidade = float(lista_entries_velocidade[i].get().strip().replace(',','.'))
                    val_dist = float(lista_entries_distancia[i].get().strip().replace(',','.'))
                    f.write(f"Velocidade {i+1},{val_velocidade},{val_dist}\n")

            f.write(f"Caminho da pasta,{caminho_pasta},\n")

    except Exception as e:
        messagebox.showerror("Erro de Arquivo", f"Não foi possível gravar o arquivo de Entrada\n{e}")
        return
    
    ########## PREENCHER AS LISTAS COM AS VELOCIDADES E DURAÇÃO OBTIDAS
    lista_velocidades_digitadas = []
    lista_duracao = []
    # Percorre os 11 itens da interface
    for i in range(11):
        
        # --- PARTE DA VELOCIDADE (Vem de um Entry -> usa .get()) ---
        texto_vel = lista_entries_velocidade[i].get().strip()
        
        # --- PARTE DA DURAÇÃO (Vem de um Label -> usa .cget("text")) ---
        texto_dur = lista_labels_duracao[i].cget("text").strip()

        # Só processa se tiver velocidade digitada E se a duração calculada for válida
        if texto_vel and texto_dur not in ["xxxx", "Erro", ""]:
            try:
                # Converte a Velocidade
                valor_vel = float(texto_vel.replace(',', '.'))
                
                # Converte a Duração (que já foi calculada em minutos no Label)
                # Se você precisar converter para segundos aqui, multiplique por 60
                valor_dur = (float(texto_dur.replace(',', '.')) * 60)

                # Adiciona nas novas listas
                lista_velocidades_digitadas.append(valor_vel)
                lista_duracao.append(valor_dur)
                
            except ValueError:
                pass # Ignora linhas com letras ou erros


    # ---------------------------------------------------------------------------------
    # INTEGRACAO COM LOGGERS EM C (MODO HEADLESS)
    # - Usa as listas lista_velocidades_digitadas + lista_duracao
    # - Converte mm/s -> RPM usando o raio informado
    # - Dispara DLG logger + Drive logger e depois faz merge
    # - Mantem o restante do GUI o mais intacto possivel
    # ---------------------------------------------------------------------------------
    if USE_EXTERNAL_RUNNER:
        try:
            raio_mm = float(ent_raio.get().strip().replace(',', '.'))
        except Exception:
            messagebox.showwarning("Valor Inválido", "Raio inválido para converter velocidade.")
            return

        # Monta schedule (rpm, duracao_s)
        schedule = []
        for vel_mm_s, dur_s in zip(lista_velocidades_digitadas, lista_duracao):
            rpm = orch.rpm_from_mm_s(vel_mm_s, raio_mm)
            schedule.append((rpm, dur_s))
        rpm_schedule = [rpm for rpm, _ in schedule]

        if not schedule:
            messagebox.showwarning("Tabela vazia", "Nenhuma etapa válida para iniciar o ensaio.")
            return

        dur_total = sum(d for _, d in schedule)
        rate_hz = float(getattr(orch, "DEFAULT_RATE_HZ", 50.0))

        # Caminhos padrao (gerados por salvar_arquivo)
        out_paths = {
            "dlg_csv": caminho_dlg_csv,
            "drive_csv": caminho_drive_csv,
            "merge_csv": caminho_merge_csv,
            "schedule_csv": caminho_schedule_csv,
        }

        # Dispara os executaveis em background
        # (repo_root = pasta do projeto; robusto para exe)
        repo_root = orch.find_repo_root()
        global external_run_state
        try:
            external_run_state = orch.start_external_run(
                repo_root=repo_root,
                out_paths=out_paths,
                schedule=schedule,
                duration_s=dur_total,
                rate_hz=rate_hz,
                com_port="COM4",
                slave_id=1,
                baud=115200,
                parity="E",
                show_console=False
            )
        except Exception as e:
            # Limpa a pasta criada se o ensaio nao iniciar
            try:
                if caminho_pasta and os.path.exists(caminho_pasta):
                    shutil.rmtree(caminho_pasta, ignore_errors=True)
            except Exception:
                pass
            messagebox.showerror("Erro ao iniciar", f"Falha ao iniciar loggers C.\n\n{e}")
            external_run_state = None
            return

        # Atualiza estado do GUI
        running = "true"
        is_paused = False
        try:
            label_ensaio_estado.config(text="Em andamento")
            _set_targets_stopped()
        except Exception:
            pass
        if 'button_frame5_pausar' in globals():
            button_frame5_pausar.config(text="Pausar", bg="SystemButtonFace")

        # Relogio do ensaio:
        # - so inicia quando o DLG validar N amostras (evita tempo correr antes do DLG).
        global timer_started
        timer_started = False

        # Thread que aguarda fim e faz merge (nao bloqueia GUI)
        state_snapshot = external_run_state
        def _wait_and_merge():
            try:
                merge_rc = orch.wait_and_merge(state_snapshot)
                if os.path.exists(caminho_merge_csv):
                    log_msg(f"Merge gerado: {caminho_merge_csv} (rc={merge_rc})")
                else:
                    log_msg("Merge nao foi gerado.")
            except Exception as e:
                log_msg(f"Erro no merge final: {e}")
            finally:
                # Atualiza estado visual ao final
                global running, is_paused, external_run_state
                running = False
                is_paused = False
                external_run_state = None
                try:
                    label_ensaio_estado.config(text="Finalizado")
                    _set_targets_stopped()
                except Exception:
                    pass
                # Limpa os graficos ao final do ensaio (evita sobreposicao no proximo)
                def _clear_graphs():
                    try:
                        for i in range(numChannels):
                            graSamps[i].clear()
                        sampsTimestamp.clear()
                        p_strokes.clear()
                        p_atrito_ef.clear()
                        p_atrito_max.clear()
                        p_atrito_min.clear()
                        p_coluna_velocidade.clear()
                        ax1.clear()
                        ax2.clear()
                        ax3.clear()
                        if ax3_twin is not None:
                            ax3_twin.clear()
                    except Exception:
                        pass
                try:
                    root.after(0, _clear_graphs)
                except Exception:
                    _clear_graphs()
        threading.Thread(target=_wait_and_merge, daemon=True).start()

        # Thread para alimentar graficos em tempo real a partir do dlg.csv
        threading.Thread(
            target=_tail_dlg_csv_for_graphs,
            args=(caminho_dlg_csv,),
            daemon=True
        ).start()

        # Thread para atualizar velocidade atual (modo externo)
        threading.Thread(
            target=_update_vel_label_from_schedule,
            args=(lista_velocidades_digitadas, lista_duracao, rpm_schedule),
            daemon=True
        ).start()
        threading.Thread(
            target=_wait_dlg_ok_and_start_timer,
            args=(caminho_dlg_csv, 3, 15),
            daemon=True
        ).start()
        return

    ########## PARTE DOS GRÁFICOS
    global tensao_vel, duracao, soma_tempos_vel, tempo_total_aqc
    global p_strokes, p_atrito_ef, p_atrito_max, p_atrito_min, contador_amostras_total

    contador_amostras_total = 0

    # Limpar as listas do terceiro gráfico
    p_strokes.clear()
    p_atrito_ef.clear()
    p_atrito_max.clear()
    p_atrito_min.clear()

    running = "true"

    '''
    qte_vel = 1
    tensao_vel = [0]
    duracao = [1000]
    soma_tempos_vel = [1000]
    tempo_total_aqc = 1000
    '''

    # Limpa buffers gráficos
    for i in range(numChannels):
        graSamps[i].clear()

    sampsTimestamp.clear()
        
    is_paused = False # Garante que começa despausado
    #button_frame5_pausar.config(text="Pausar", bg="SystemButtonFace")
    if 'button_frame5_pausar' in globals(): # Reseta o texto do botão se ele já existir
         button_frame5_pausar.config(text="Pausar", bg="SystemButtonFace")

    # Inicia Thread
    _start_timer_now()
    threading.Thread(target=go, daemon=True).start()
    threading.Thread(target=go_p, daemon=True).start()
    threading.Thread(target=velocidades, daemon=True).start()
    # atualiza_decorrido() ja chamado em _start_timer_now()


def pause_acquisition():
    global is_paused, start_time, tempo_pause_inicio, running
    
    # Só funciona se a aquisição estiver rodando (running == "true")
    if running == "true":
        # --------------------------------------------------------------
        # INTEGRACAO COM LOGGERS EM C:
        # Pausar ainda não está implementado no pipeline externo.
        # (Evita estado inconsistente de loggers externos.)
        # --------------------------------------------------------------
        if USE_EXTERNAL_RUNNER:
            messagebox.showwarning("Pausar não disponível",
                                   "O modo externo (DLG + Drive) ainda não suporta pausa.\n"
                                   "Use 'Parar' para finalizar o ensaio.")
            return
        if not is_paused:
            
            is_paused = True
            tempo_pause_inicio = time.time() # Marca a hora que pausou
            
            # Muda o texto do botão visualmente
            button_frame5_pausar.config(text="Retomar", bg="yellow")
            print("Aquisição PAUSADA.")
            
        else:
        
            is_paused = False
            tempo_agora = time.time()
            tempo_que_ficou_parado = tempo_agora - tempo_pause_inicio
            
            start_time = start_time + tempo_que_ficou_parado
            
            # Muda o botão de volta
            button_frame5_pausar.config(text="Pausar", bg="SystemButtonFace")
            print("Aquisição RETOMADA.")
            
            # Reinicia o loop do relógio na tela
            atualiza_decorrido()


def stop_acquisition():
    global running, is_paused, ip, timer_started
    global graSamps, sampsTimestamp, p_strokes, p_atrito_ef, p_atrito_max, p_atrito_min, p_coluna_velocidade

    if running:
        resposta = messagebox.askyesno("Confirmação", "Deseja parar o programa?")
        if resposta:
            running = False
            is_paused = False

            # --------------------------------------------------------------
            # INTEGRACAO COM LOGGERS EM C:
            # Se o modo externo estiver ativo, finalizamos os processos aqui.
            # --------------------------------------------------------------
            global external_run_state
            if USE_EXTERNAL_RUNNER and external_run_state is not None:
                orch.stop_run(external_run_state)
                external_run_state = None
                try:
                    label_ensaio_estado.config(text="Aguardando novo ensaio")
                    _set_targets_stopped()
                except Exception:
                    pass
                lbl_tempo_decorrido2.config(text="0:00:00")
                timer_started = False
                if 'button_frame5_pausar' in globals():
                    button_frame5_pausar.config(text="Pausar", bg="SystemButtonFace")
                return

            if 'button_frame5_pausar' in globals():
                button_frame5_pausar.config(text="Pausar", bg="SystemButtonFace")

            lbl_tempo_decorrido2.config(text="0:00:00")
            try:
                label_ensaio_estado.config(text="Aguardando novo ensaio")
                _set_targets_stopped()
            except Exception:
                pass
            timer_started = False

            # Segurança: Zera a tensão da máquina imediatamente
            # (Caso a thread de velocidade demore 0.1s para morrer, garantimos aqui)
            try:
                myModule.SetAnOutV(ip, 0, 0, 100)
            except:
                pass

            ###########################
            ###########################
            ###########################
            # Limpa os dados da memória
            for i in range(numChannels):
                graSamps[i].clear()
            sampsTimestamp.clear()
            
            p_strokes.clear()
            p_atrito_ef.clear()
            p_atrito_max.clear()
            p_atrito_min.clear()
            p_coluna_velocidade.clear()

            # Limpa os eixos imediatamente
            ax1.clear()

# Nicolas
# update1(frame): Limpa o ax1, verifica os canais ativos, processa o tempo baseada na frequência e plota o gráfico (ax1.plot) da temperatura.
# Ela é chamada automaticamente e repetidamente pelo Matplotlib através da ferramenta de animação no fim do código.
def update1(frame):
    # Acessa a lista de tempos reais
    global sampsTimestamp 

    if not _is_running() or not graSamps: 
        return

    # Apaga tudo o que foi desenhado no quadro anterior para desenhar o novo.
    ax1.clear()

    # Regras fixas (validacao):
    # - Grafico 1 (temperatura) usa CH2 do DLG.
    # Mantemos os checkboxes, mas este grafico respeita apenas o canal 2.
    c2 = canalativo2.get()

    # calcula quantos minutos cabem na escala que o usuário digitou no eixo x:
    # (Total de Amostras 'aux' / Frequência '200') / 60 segundos
    limite_tela_min = aux / freq / 60
    passo = 5 # Desenha só 1 ponto a cada 5. Deixa o programa mais leve

    # Função auxiliar para não repetir código e transformar o tempo absoluto em relativo (Sweep).
    def plotar_canal(idx, cor, label):
        if len(graSamps) > idx:
            raw_dados = graSamps[idx]
            # Garante que não vai acessar índices inexistentes
            tam = min(len(sampsTimestamp), len(raw_dados))
            
            if tam > 0:
                # Pega os dados sincronizados, cortando as listas para ficarem do mesmo tamanho
                t_data = np.array(sampsTimestamp[:tam])
                y_data = np.array(raw_dados[:tam])
                
                # --- TRANSFORMA TEMPO ABSOLUTO EM RELATIVO ---
                # O 'sampsTimestamp' guarda o tempo real.
                # Mas quando a tela limpa, o gráfico deve começar do 0 visualmente.
                # Por isso é feito: TempoAtual - TempoDoPrimeiroPonto
                if len(t_data) > 0:
                    t_zero = t_data[0] # Pega o tempo do primeiro ponto atual

                    # Calcula o eixo X
                    tempo = ((t_data - t_zero) / 60.0)[::passo]
                    dados = y_data[::passo]
                    
                    # Manda desenhar a linha
                    if cor:
                        ax1.plot(tempo, dados, label=label, color=cor)
                    else:
                        ax1.plot(tempo, dados, label=label)

    # CH2 (temperatura) = index 1 na lista graSamps.
    if c2:
        plotar_canal(1, None, 'Channel 2')

    # Configuração Visual
    handles, labels = ax1.get_legend_handles_labels()
    # Se tiver alguma linha desenhada, mostra a legenda no canto superior direito
    if handles: 
        ax1.legend(loc='upper right', fontsize='small')

    ax1.set_xlim(0, limite_tela_min)
    ax1.grid(alpha=0.5, lw=0.5)
    ax1.set_facecolor('black')
    ax1.tick_params(axis='both', labelsize=8)
    ax1.set_xlabel('Tempo [min]', fontsize=9)
    ax1.set_ylabel('Temperatura [°C]', fontsize=9)

    # Controle do eixo y
    if not y1_auto:
        ax1.set_ylim(y1_min, y1_max)

def update2(frame):
    global sampsTimestamp

    if not _is_running() or not graSamps: 
        return

    ax2.clear()
    # Regras fixas (validacao):
    # - Grafico 2 (forca) usa CH1 do DLG.
    c1 = canalativo1.get()
    limite_tela_min = aux / freq / 60
    passo = 5

    # Canal 1 (Forca)
    if c1 == 1 and len(graSamps) > 0:
        # Pega a lista bruta de dados de força
        raw_dados = graSamps[0]
        tam = min(len(sampsTimestamp), len(raw_dados))
        
        if tam > 0:
            t_data = np.array(sampsTimestamp[:tam])
            y_data = np.array(raw_dados[:tam])

            if len(t_data) > 0:
                # Mesmo ajuste de tempo aqui
                # Pega o instante do primeiro ponto que está na tela
                t_zero = t_data[0]
                # Subtrai o t_zero de todos os pontos. 
                # Isso faz o gráfico sempre começar visualmente no 0
                tempo = ((t_data - t_zero) / 60.0)[::passo]
                dados = y_data[::passo]
                
                ax2.plot(tempo, dados, label='Channel 1', color='#1f77b4') # Desenha a linha azul

    # Configuração Visual Ax2
    handles, labels = ax2.get_legend_handles_labels()
    if handles: 
        ax2.legend(loc='upper right', fontsize='small')
    
    ax2.set_xlim(0, limite_tela_min)
    ax2.grid(alpha=0.5, lw=0.5)
    ax2.set_facecolor('black')
    ax2.tick_params(axis='both', labelsize=8)
    ax2.set_xlabel('Tempo [min]', fontsize=9)
    ax2.set_ylabel('Força de atrito [N]', fontsize=9)
    
    if not y2_auto:
        ax2.set_ylim(y2_min, y2_max)


# Nicolas
# Limpa o ax3, gerencia o segundo eixo y (ax3_twin) e plota as curvas de atrito e velocidade
def update3(frame):
    global ax3_twin
    if not _is_running(): 
        return

    ax3.clear() # Limpa apenas o gráfico 3
    
    # Limpa ou cria o eixo y da direita
    if ax3_twin is None:
        ax3_twin = ax3.twinx() # Cria um eixo y à direita
    ax3_twin.clear()

    # Desenha 1 a cada 5 pontos.
    passo = 5

    if len(p_strokes) > 0:
        # Plota três linhas de atrito (Eixo Esquerdo)
        ax3.plot(p_strokes[::passo], p_atrito_max[::passo], color='red', label='µmax', linewidth=1)
        ax3.plot(p_strokes[::passo], p_atrito_ef[::passo], color='black', label='µef', linewidth=1)
        ax3.plot(p_strokes[::passo], p_atrito_min[::passo], color='cyan', label='µmin', linewidth=1)

        # Plota velocidade (Eixo Direito - Coluna 10 do arquivo P)
        if len(p_coluna_velocidade) > 0:
            # Garante que X (Strokes) e Y (Velocidade) tenham o mesmo tamanho
            tam = min(len(p_strokes), len(p_coluna_velocidade))
            # Plota o eixo da Velocidade
            ax3_twin.plot(p_strokes[:tam][::passo], p_coluna_velocidade[:tam][::passo], color='orange', label='Vel', linewidth=2)
            
            # Ajusta escala Y da direita se a velocidade aumenta bastante
            max_v = max(p_coluna_velocidade) if p_coluna_velocidade else 10
           
            if max_v <= 0: max_v = 10
            ax3_twin.set_ylim(0, max_v * 1.1) # Define o limite y da velocidade com uma folga de 10% no topo

        # Ajusta Eixo X para seguir os Strokes
        # Faz o gráfico acompanhar o crescimento dos strokes.
        # Define o mínimo como 0 e o máximo como o último stroke registrado.
        if p_strokes:
            ax3.set_xlim(0, max(p_strokes[-1], 100))

    # --- CONFIGURAÇÃO VISUAL AX3 ---
    # Eixo y esquerdo
    ax3.set_xlabel('Strokes', fontsize=9)
    ax3.set_ylabel('Coef. Atrito [-]', fontsize=9)
    ax3.grid(True, alpha=0.5)
    ax3.tick_params(axis='both', labelsize=8)
    ax3.set_facecolor('white')

    # Eixo y direito
    ax3_twin.set_ylabel('Vel', fontsize=9, color='black')
    ax3_twin.yaxis.set_label_position("right") # Garante que o texto Vel fique na direita
    ax3_twin.yaxis.tick_right()
    ax3_twin.tick_params(axis='y', labelcolor='black', labelsize=8)

    # Junta as legendas do ax3 (atrito) e do ax3_twin (velocidade)
    h1, l1 = ax3.get_legend_handles_labels()
    h2, l2 = ax3_twin.get_legend_handles_labels()
    
    if h1 or h2:
        # Soma as listas (h1+h2) e cria uma legenda única.
        ax3.legend(h1+h2, l1+l2, loc='upper center', bbox_to_anchor=(0.5, -0.25), 
                   fontsize='small', ncol=4)



def get_ip():
    pass
    '''
    global ip  # Declare ip as a global variable
    ip = receive_ip_entry.get()  # Update the global variable with the entry value
    print(f"IP received: {ip}")  # Print the IP value after it's been updated
    with open('inicio.txt', 'w') as f:  # Modo 'w' para escrever
        f.write(ip)
'''

"""
def get_nome():
    global nome  # Declare ip as a global variable
    nome = receive_nome_entry.get()  # Update the global variable with the entry value
    print(f"nome received: {nome}")  # Print the IP value after it's been updated
"""


def get_aux():
    global aux
    try:
        aux = int(aux_spinbox.get())
    except:
        aux_label = tkinter.Label(labelframe1_lb1_3, text=f"configurado:{aux} ", fg="black", bg="white")
        aux_label.grid(row=3, column=3, sticky="nw")



def alterar_escala_tempo():
    """Atualiza a escala do eixo X (min) convertendo para numero de amostras (aux)."""
    global aux
    try:
        txt = entry_escala_x.get().strip().replace(",", ".")
        if not txt:
            return
        minutos = float(txt)
        if minutos <= 0:
            raise ValueError()
        novo_aux = int(minutos * freq * 60)
        if novo_aux < 1:
            novo_aux = 1
        aux = novo_aux
        try:
            aux_spinbox.delete(0, "end")
            aux_spinbox.insert(0, str(aux))
        except Exception:
            pass
        log_msg(f"Escala X ajustada: {minutos:.2f} min -> aux={aux}")
    except Exception:
        messagebox.showwarning("Valor Invalido", "Escala do eixo X deve ser um numero positivo (minutos).")

def abrir_config_y():
    """Stub simples para configuracao de eixos Y (mantem UI funcional)."""
    messagebox.showinfo("Configurar Eixos Y", "Configuracao de eixos Y ainda nao implementada neste executavel.")
def aumenta_tensao():
    if not HAVE_LDTP:
        messagebox.showwarning('Indisponivel', 'Modulo LDTP nao encontrado. Controle analogico interno desativado.')
        return
    
    pass
    '''
    global tensao
    tensao = tensao + 0.02
    myModule.SetAnOutV(ip, tensao, 0, 100)
    '''

def diminui_tensao():
    if not HAVE_LDTP:
        messagebox.showwarning('Indisponivel', 'Modulo LDTP nao encontrado. Controle analogico interno desativado.')
        return
    
    pass
    '''
    global tensao
    tensao = tensao - 0.02
    myModule.SetAnOutV(ip, tensao, 0, 100)
    '''

# Guia "supervisorio" desativada; mantemos stub para evitar NameError.
def get_channels():
    pass

# variáveis globais
ip = '190.29.92.63'
timer = 0
tempo_total_aqc = 0
freq = 50.0
taxagraficos = int(1 / freq)
active_abas = 0
cont_abas = 0
filePath = 'dadoslatrib.txt'
filePath2 = 'dadoslatrib2.txt'
running = False  # Flag para controlar a aquisição de dados
horas = 0
minutos = 0
segundos = 0
gotSamples = False
nome = ''
caminho_arquivo = ''
caminho_arquivo2 = ''
aux = 10000
start_time = 0
tempo_decorrido = 0
running = False
is_paused = False    
tempo_pause_inicio = 0

tensao_vel = []
qte_vel = 0
duracao = []
soma_tempos_vel = []
soma_duracoes = 0









#############   tkinter

# cria interface gráfica
root = tkinter.Tk()
root.title("Software reciprocating LATRIB")
# root.geometry('600x600')

# Canais fixos: 8 canais analogicos habilitados por padrao.
# (Canal 0/digital fica desativado.)
canalativo0 = tkinter.IntVar(value=0)
canalativo1 = tkinter.IntVar(value=1)
canalativo2 = tkinter.IntVar(value=1)
canalativo3 = tkinter.IntVar(value=1)
canalativo4 = tkinter.IntVar(value=1)
canalativo5 = tkinter.IntVar(value=1)
canalativo6 = tkinter.IntVar(value=1)
canalativo7 = tkinter.IntVar(value=1)
canalativo8 = tkinter.IntVar(value=1)


# Tamanho da janela
width = 1500 #650
height = 800 #1000

# Obtendo a resolução da tela (largura e altura)
screen_width = root.winfo_screenwidth()
screen_height = root.winfo_screenheight()

x_pos = (screen_width - width) // 2
y_pos = 0

# Tamanho e posição da janela
root.geometry(f"{width}x{height}+{x_pos}+{y_pos}")


# Novo layout principal:
# Frame principal:
main_frame = tkinter.Frame(root)
main_frame.pack(fill='both', expand=True)

# Frame gráficos
frame_graficos = tkinter.Frame(main_frame, bg='gray')
frame_graficos.pack(side='left', fill='both', expand=False)

# Frame abas de controle
frame_abas = tkinter.Frame(main_frame)
frame_abas.pack(side='right', fill='both', expand=True)

# cria abas
nb = ttk.Notebook(frame_abas)
nb.pack(expand="true", fill="both")

###
###
# aba2 das configurações
aba2 = tkinter.Frame(nb)
aba2.grid(sticky="news", row=0, column=0)
aba2.grid_rowconfigure(0, weight=1)
aba2.grid_columnconfigure(0, weight=1)
aba2.grid_rowconfigure(1, weight=1)
aba2.grid_rowconfigure(2, weight=1)

nb.add(aba2, text="configurações iniciais")

# ------------------------------------------------------------------
# ABA DE LOG (DESENVOLVIMENTO)
# ------------------------------------------------------------------
# Esta aba serve para debug durante desenvolvimento e validacao de
# comunicacao (ex.: Check status). O log e somente informativo.
aba_log = tkinter.Frame(nb)
aba_log.grid(sticky="news", row=0, column=0)
aba_log.grid_rowconfigure(0, weight=1)
aba_log.grid_columnconfigure(0, weight=1)
aba_log.grid_columnconfigure(1, weight=0)

nb.add(aba_log, text="log")

# Text widget com scroll vertical para mensagens de debug.
log_text = tkinter.Text(aba_log, height=10, wrap="none", state="disabled")
log_text.grid(row=0, column=0, sticky="nsew", padx=(6, 0), pady=6)

log_scroll_y = tkinter.Scrollbar(aba_log, orient="vertical", command=log_text.yview)
log_scroll_y.grid(row=0, column=1, sticky="ns", padx=(2, 6), pady=6)

log_text.configure(yscrollcommand=log_scroll_y.set)

#'''''''''''''''''''''''''''''''''''''''''''''''''''''''''''''''''

############# ''''''''''''''''''''''' lbf DE CIMA::"""""""""""""""""

###### SUPERIOR ESQUERDO

labelframe1_aba2 = tkinter.LabelFrame(aba2)
labelframe1_aba2.grid(sticky="nwes", row=0, column=0)
labelframe1_aba2.grid_columnconfigure(0, weight=1)
labelframe1_aba2.grid_columnconfigure(1, weight=1)
#labelframe1_aba2.grid_rowconfigure(1, weight=1)
labelframe1_aba2.grid_rowconfigure(0, weight=1)

label1_esq = tkinter.LabelFrame(labelframe1_aba2)
label1_esq.grid(sticky="nwes", row=0, column=0)
label1_esq.grid_rowconfigure(0, weight=1)
label1_esq.grid_rowconfigure(8, weight=1)
label1_esq.grid_columnconfigure(0, weight=2)
label1_esq.grid_columnconfigure(1, weight=2)
label1_esq.grid_columnconfigure(2, weight=1)


nomes_esq = [
    "Nome do ensaio",
    "Estudo",
    "Repetição",
    "Diâmetro da esfera [mm]",
    "Material da esfera",
    "Material do bloco",
    "Corpo de prova (bloco)"
]

entries_left = {}

for i, texto in enumerate(nomes_esq):
    lbl_primeira_tabela = tkinter.Label(label1_esq, text=texto, font=("Arial, 10"))
    lbl_primeira_tabela.grid(row=i+1, column=0, sticky="news", padx=5, pady=2)

    ent = tkinter.Entry(label1_esq)
    ent.grid(row=i+1, column=1, sticky="ew", padx=5, pady=2)

    label1_esq.grid_rowconfigure(i+1, weight=0)
    entries_left[texto] = ent



###### SUPERITOR DIREITO
label2_dir = tkinter.LabelFrame(labelframe1_aba2, text="")
label2_dir.grid(row=0, column=1, sticky="news", padx=10, pady=1)
label2_dir.grid_columnconfigure(0, weight=1)
label2_dir.grid_columnconfigure(1, weight=1)
label2_dir.grid_columnconfigure(2, weight=1)
label2_dir.grid_columnconfigure(3, weight=5)
label2_dir.grid_rowconfigure(1, weight=1)
label2_dir.grid_rowconfigure(9, weight=3)

# Checkbox Lubrificado
lubrificado_var = tkinter.BooleanVar()

chk_lub = tkinter.Checkbutton(
    label2_dir,
    text="Lubrificado",
    variable=lubrificado_var,
    bg="#F0F0F0",
    fg="black",
    font=("Arial", 10)
)
chk_lub.grid(row=0, column=0, sticky="w", padx=5, pady=5)

# Cabeçalhos
tkinter.Label(label2_dir, text="Material", font=("Arial", 10)).grid(row=2, column=1, sticky="ew", padx=5)
tkinter.Label(label2_dir, text="%", font=("Arial", 10)).grid(row=2, column=2, sticky="ew", padx=5)

lista_entries_material = []
lista_entries_porcentagem = []

for i in range(5):
    ent_mat = tkinter.Entry(label2_dir)
    ent_mat.grid(row=i + 3, column=1, sticky="ew", padx=5, pady=5)
    lista_entries_material.append(ent_mat) # Guarda na lista

    ent_pct = tkinter.Entry(label2_dir, width=6)
    ent_pct.grid(row=i + 3, column=2, sticky="ew", padx=5, pady=5)
    lista_entries_porcentagem.append(ent_pct) # Guarda na lista

    label2_dir.grid_rowconfigure(i + 2, weight=0)

def muda_estado_lub():
    esta_lubrificado = lubrificado_var.get()
    
    # 1. Muda a cor do checkbox
    chk_lub.config(selectcolor="blue" if esta_lubrificado else "white")
    
    # 2. Define o estado (normal ou disabled)
    novo_estado = 'normal' if esta_lubrificado else 'disabled'
    
    # 3. Aplica em todas as caixas das listas
    for widget in lista_entries_material:
        widget.config(state=novo_estado)
    for widget in lista_entries_porcentagem:
        widget.config(state=novo_estado)
    '''
    if lubrificado_var.get():
        chk_lub.config(selectcolor="blue")
    else:
        chk_lub.config(selectcolor="white")

    '''

chk_lub.config(command=muda_estado_lub)
muda_estado_lub()



#'''''''''''''''''''''''''''''''''''''''''''''''''''''''''''''''''

############# ''''''''''''''''''''''' lbf DE BAIXO::"""""""""""""""""
'''
labelframe1_lb2 = tkinter.LabelFrame(aba2, text = "")
labelframe1_lb2.grid(sticky="news", row=1, column=0)
labelframe1_lb2.grid_rowconfigure(0, weight=1)
labelframe1_lb2.grid_columnconfigure(0, weight=1)
'''


###### PRIMEIRO INFERIOR
labelframe1_baixo = tkinter.LabelFrame(aba2)
labelframe1_baixo.grid(sticky="news", row=1, column=0)
labelframe1_baixo.grid_rowconfigure(0, weight=2)
labelframe1_baixo.grid_rowconfigure(1, weight=0)
labelframe1_baixo.grid_rowconfigure(2, weight=0)
labelframe1_baixo.grid_rowconfigure(3, weight=2)
labelframe1_baixo.grid_columnconfigure(0, weight=1)
labelframe1_baixo.grid_columnconfigure(1, weight=1)
labelframe1_baixo.grid_columnconfigure(2, weight=3)
labelframe1_baixo.grid_columnconfigure(3, weight=1)
labelframe1_baixo.grid_columnconfigure(4, weight=1)
labelframe1_baixo.grid_columnconfigure(5, weight=6)
#labelframe1_baixo.grid_columnconfigure(1, weight=1)

entries_superiores = {}

# Raio [mm]
lbl_raio = tkinter.Label(labelframe1_baixo, text="Raio [mm]", font=("Arial", 10))
lbl_raio.grid(row=1, column=0, padx=2, pady=2, sticky="ew")

ent_raio = tkinter.Entry(labelframe1_baixo, width= 12)
ent_raio.grid(row=1, column=1, padx=2, pady=2, sticky="w")
entries_superiores["Raio [mm]"] = ent_raio

# Força normal [N]
lbl_forca = tkinter.Label(labelframe1_baixo, text="Força normal [N]", font=("Arial", 10))
lbl_forca.grid(row=2, column=0, padx=2, pady=2, sticky="ew")

ent_forca = tkinter.Entry(labelframe1_baixo, width=12)
ent_forca.grid(row=2, column=1, padx=2, pady=2, sticky="w")
entries_superiores["Força normal [N]"] = ent_forca

#checkbox reciprocante
reciprocante_var = tkinter.BooleanVar()

chk_reciprocante = tkinter.Checkbutton(
    labelframe1_baixo,
    text="Reciprocating",
    variable=reciprocante_var,
    bg="#F0F0F0",
    fg="black",
    font=("Arial", 10)
)
chk_reciprocante.grid(row=1, column=2, padx=5, pady=5, sticky="e")

# ----- Curso -----
lbl_curso = tkinter.Label(labelframe1_baixo, text="Curso [mm]", font=("Arial", 10))
lbl_curso.grid(row=1, column=3, sticky="e")
entry_curso = tkinter.Entry(labelframe1_baixo, width=10)
entry_curso.grid(row=1, column=4, padx=5, sticky="w")

def muda_estado_reciprocante():
    if reciprocante_var.get():
        chk_reciprocante.config(selectcolor="blue")
        entry_curso.config(state='normal')

        if 'lbl_header_voltas_cursos' in globals():
            lbl_header_voltas_cursos.config(text="Cursos")
    else:
        chk_reciprocante.config(selectcolor="white")
        entry_curso.config(state='disabled')
        
        if 'lbl_header_voltas_cursos' in globals():
            lbl_header_voltas_cursos.config(text='Voltas')

    if 'calcular_voltas_cursos_duracao' in globals():
        calcular_voltas_cursos_duracao()


def calcular_voltas_cursos_duracao(event=None):
    """
    Atualiza as colunas Voltas/Cursos e Duracao com base em:
    - Velocidade [mm/s]
    - Distancias [mm]
    - Raio [mm] (modo rotativo)
    - Curso [mm] (modo reciprocante)
    """
    try:
        raio_txt = ent_raio.get().strip().replace(",", ".")
        raio = float(raio_txt) if raio_txt else None
    except Exception:
        raio = None

    try:
        curso_txt = entry_curso.get().strip().replace(",", ".")
        curso = float(curso_txt) if curso_txt else None
    except Exception:
        curso = None

    for i in range(11):
        vel_txt = lista_entries_velocidade[i].get().strip().replace(",", ".")
        dist_txt = lista_entries_distancia[i].get().strip().replace(",", ".")

        # Se ambos vazios, limpa indicadores
        if not vel_txt and not dist_txt:
            lista_labels_voltas_cursos[i].config(text="xxxx")
            lista_labels_duracao[i].config(text="xxxx")
            continue

        try:
            vel = float(vel_txt) if vel_txt else None
            dist = float(dist_txt) if dist_txt else None
        except Exception:
            lista_labels_voltas_cursos[i].config(text="Erro")
            lista_labels_duracao[i].config(text="Erro")
            continue

        # Duracao [min]
        if vel is None or dist is None or vel <= 0:
            lista_labels_duracao[i].config(text="Erro")
        else:
            dur_min = (dist / vel) / 60.0
            lista_labels_duracao[i].config(text=f"{dur_min:.2f}")

        # Voltas ou Cursos
        if reciprocante_var.get():
            if curso is None or curso <= 0 or dist is None:
                lista_labels_voltas_cursos[i].config(text="Erro")
            else:
                cursos = dist / curso
                lista_labels_voltas_cursos[i].config(text=f"{cursos:.2f}")
        else:
            if raio is None or raio <= 0 or dist is None:
                lista_labels_voltas_cursos[i].config(text="Erro")
            else:
                voltas = dist / (2.0 * 3.141592653589793 * raio)
                lista_labels_voltas_cursos[i].config(text=f"{voltas:.2f}")
chk_reciprocante.config(command=muda_estado_reciprocante)



###### SEGUNDO INFERIOR

labelframe1_2_baixo = tkinter.LabelFrame(aba2)
labelframe1_2_baixo.grid(sticky="news", row=2, column=0)
labelframe1_2_baixo.grid_rowconfigure(0, weight=1)
labelframe1_2_baixo.grid_columnconfigure(0, weight=1)
labelframe1_2_baixo.grid_columnconfigure(1, weight=1)

labelframe2_baixo = tkinter.LabelFrame(labelframe1_2_baixo)
labelframe2_baixo.grid(sticky="news", row=0, column=0)
labelframe2_baixo.grid_rowconfigure(0, weight=2)
labelframe2_baixo.grid_rowconfigure(12, weight=10)
labelframe2_baixo.grid_columnconfigure(0, weight=1)
labelframe2_baixo.grid_columnconfigure(1, weight=1)
labelframe2_baixo.grid_columnconfigure(2, weight=1)
labelframe2_baixo.grid_columnconfigure(3, weight=1)
labelframe2_baixo.grid_columnconfigure(4, weight=5)
labelframe2_baixo.grid_columnconfigure(5, weight=3)

labelframe3_baixo = tkinter.LabelFrame(labelframe1_2_baixo)
labelframe3_baixo.grid(sticky="news", row=0, column=1)
labelframe3_baixo.grid_rowconfigure(0, weight=1)
labelframe3_baixo.grid_rowconfigure(1, weight=0)
labelframe3_baixo.grid_rowconfigure(2, weight=0)
labelframe3_baixo.grid_rowconfigure(3, weight=1)
labelframe3_baixo.grid_rowconfigure(4, weight=0)
labelframe3_baixo.grid_rowconfigure(5, weight=0)
labelframe3_baixo.grid_rowconfigure(6, weight=0)
# Mantem altura consistente dos botoes; evita "gordura" no Check status.
labelframe3_baixo.grid_rowconfigure(7, weight=0)
labelframe3_baixo.grid_rowconfigure(8, weight=0)
labelframe3_baixo.grid_columnconfigure(0, weight=1)
labelframe3_baixo.grid_columnconfigure(1, weight=1)


#Cabeçalhos da parte de baixo
tkinter.Label(labelframe2_baixo, text="Velocidade [mm/s]", font=("Arial", 9, "bold")) \
    .grid(row=0, column=0, padx=5, pady=5)
tkinter.Label(labelframe2_baixo, text="Distâncias [mm]", font=("Arial", 9, "bold")) \
    .grid(row=0, column=1, padx=5, pady=5)

global lbl_header_voltas_cursos
lbl_header_voltas_cursos = tkinter.Label(labelframe2_baixo, text="Voltas", font=("Arial", 9, "bold"))
lbl_header_voltas_cursos.grid(row=0, column=2, padx=5, pady=5)
    
tkinter.Label(labelframe2_baixo, text="Duração [min]", font=("Arial", 9, "bold")) \
    .grid(row=0, column=3, padx=5, pady=5)
tkinter.Label(labelframe3_baixo, text="Estado", font=("Arial", 9, "bold")).grid(row=0, column=0, padx=5, pady=5)
label_ensaio_estado = tkinter.Label(labelframe3_baixo, text="Aguardando novo ensaio", font=("Arial", 9))
label_ensaio_estado.grid(row=0, column=1, padx=5, pady=5)



for i in range(11): #### Velocidade

    ent = tkinter.Entry(labelframe2_baixo,  width=10)
    ent.grid(row=i+1, column=0, padx=2, pady=2)
    lista_entries_velocidade.append(ent)

    #entries_superiores[texto] = ent

for i in range(11): #### Distâncias

    ent = tkinter.Entry(labelframe2_baixo,  width=10)
    ent.grid(row=i+1, column=1, padx=2, pady=2)
    lista_entries_distancia.append(ent)


for i in range(11):
    voltas_cursos_lbl = tkinter.Label(labelframe2_baixo, text="xxxx")
    voltas_cursos_lbl.grid(row=1 + i, column=2, sticky="news", padx=5)
    lista_labels_voltas_cursos.append(voltas_cursos_lbl
                                      )
    duracao_lbl = tkinter.Label(labelframe2_baixo, text="xxxx")
    duracao_lbl.grid(row=1 + i, column=3, sticky="news", padx=5)
    lista_labels_duracao.append(duracao_lbl)


for entry in lista_entries_velocidade:
    entry.bind('<KeyRelease>', calcular_voltas_cursos_duracao)

for entry in lista_entries_distancia:
    entry.bind('<KeyRelease>', calcular_voltas_cursos_duracao)

ent_raio.bind('<KeyRelease>', calcular_voltas_cursos_duracao)
entry_curso.bind('<KeyRelease>', calcular_voltas_cursos_duracao)

muda_estado_reciprocante()

### Tempo, velocidade e botões
lbl_vel_atual = tkinter.Label(labelframe3_baixo, text="Velocidade alvo atual", font=("Arial", 9, "bold"))
lbl_vel_atual.grid(row=1, column=0, padx=5, pady=5)
label_ensaio_vel = tkinter.Label(labelframe3_baixo, text="Parado", font=("Arial", 12))
label_ensaio_vel.grid(row=1, column=1, padx=10, pady=10)
lbl_rpm_alvo = tkinter.Label(labelframe3_baixo, text="RPM Alvo atual", font=("Arial", 9, "bold"))
lbl_rpm_alvo.grid(row=2, column=0, padx=5, pady=5)
label_ensaio_rpm = tkinter.Label(labelframe3_baixo, text="0 rpm", font=("Arial", 12))
label_ensaio_rpm.grid(row=2, column=1, padx=10, pady=10)
lbl_tempo_decorrido1 = tkinter.Label(labelframe3_baixo, text="Tempo restante", font=("Arial", 9, "bold"))
lbl_tempo_decorrido1.grid(row=3, column=0, padx=5, pady=5)
lbl_tempo_decorrido2 = tkinter.Label(labelframe3_baixo, text="0:00:00", font=("Arial", 40, "bold"))
lbl_tempo_decorrido2.grid(row=3, column=1, sticky="w", padx=5, pady=5)

#Botões
button_frame4_iniciar = tkinter.Button(labelframe3_baixo, text="Iniciar", command=start_acquisition)
button_frame4_iniciar.grid(sticky="news", row=5, column=0,columnspan=2, padx=5, pady=2)
button_frame5_pausar = tkinter.Button(labelframe3_baixo, text="Pausar", command=pause_acquisition)
button_frame5_pausar.grid(sticky="news", row=6, column=0,columnspan=2, padx=5, pady=2)
button_frame5_parar = tkinter.Button(labelframe3_baixo, text="Parar", command=stop_acquisition)
button_frame5_parar.grid(sticky="news", row=7, column=0,columnspan=2, padx=5, pady=2)

# Check status (DLG + Drive) - separado dos botoes principais
button_frame6_status = tkinter.Button(labelframe3_baixo, text="Check status", command=check_status)
button_frame6_status.grid(sticky="news", row=8, column=0, columnspan=2, padx=5, pady=(10, 2))

status_frame = tkinter.Frame(labelframe3_baixo)
status_frame.grid(sticky="w", row=9, column=0, columnspan=2, padx=5, pady=(2, 6))
tkinter.Label(status_frame, text="DLG:", font=("Arial", 9, "bold")).grid(row=0, column=0, padx=(0, 4))
status_dlg_value = tkinter.Label(status_frame, text="?", fg="gray")
status_dlg_value.grid(row=0, column=1, padx=(0, 12))
tkinter.Label(status_frame, text="Drive:", font=("Arial", 9, "bold")).grid(row=0, column=2, padx=(0, 4))
status_drive_value = tkinter.Label(status_frame, text="?", fg="gray")
status_drive_value.grid(row=0, column=3, padx=(0, 4))
'''
atualiza_tempo_button = tkinter.Button(labelframe2_lb2_4, text="atualiza", font=("Arial", 10),
                                       command=atualiza_decorrido)
atualiza_tempo_button.grid(row=1, column=0, padx=10, pady=10)

label_tempo_decorrido = tkinter.Label(labelframe2_lb2_4, text="tempo", font=("Arial", 10))
label_tempo_decorrido.grid(row=2, column=0, padx=10, pady=10)

label_ensaio_vel = tkinter.Label(labelframe2_lb2_4, text="vel.atual", font=("Arial", 10))
label_ensaio_vel.grid(row=3, column=0, padx=10, pady=10)
'''

########################################################################################

#####################supervisorio(aba4)

aba4 = tkinter.Frame(nb)
# aba4.grid(...) desativado para ocultar a guia supervisorio
aba4.grid_rowconfigure(0, weight=1)
aba4.grid_columnconfigure(0, weight=1)
aba4.grid_rowconfigure(1, weight=1)
aba4.grid_columnconfigure(0, weight=1)
aba4.grid_columnconfigure(1, weight=1)

# Aba "supervisorio" desativada (canais fixos e sem intertravamento).

labelframe1_aba4 = tkinter.LabelFrame(aba4, text="dados", font=("Arial", 16, "bold"))
labelframe1_aba4.grid(sticky="news", row=0, column=0)
labelframe1_aba4.grid_rowconfigure(0, weight=1)
labelframe1_aba4.grid_columnconfigure(0, weight=1)
labelframe1_aba4.grid_rowconfigure(1, weight=1)
labelframe1_aba4.grid_rowconfigure(3, weight=1)
labelframe1_aba4.grid_rowconfigure(4, weight=1)

labelframe1_lb1_4 = tkinter.LabelFrame(labelframe1_aba4, text="arquivo", font=("Arial", 12, "bold"))
labelframe1_lb1_4.grid(sticky="news", row=0, column=0)
labelframe1_lb1_4.grid_rowconfigure(0, weight=1)
labelframe1_lb1_4.grid_columnconfigure(0, weight=1)
labelframe1_lb1_4.grid_rowconfigure(1, weight=1)
labelframe1_lb1_4.grid_columnconfigure(1, weight=1)
labelframe1_lb1_4.grid_rowconfigure(2, weight=1)
labelframe1_lb1_4.grid_rowconfigure(3, weight=1)
labelframe1_lb1_4.grid_rowconfigure(4, weight=1)

labelframe1_lb2_4 = tkinter.LabelFrame(labelframe1_aba4, text="informações do ensaio", font=("Arial", 12, "bold"))
labelframe1_lb2_4.grid(sticky="news", row=1, column=0)
labelframe1_lb2_4.grid_rowconfigure(0, weight=1)
labelframe1_lb2_4.grid_columnconfigure(0, weight=1)
labelframe1_lb2_4.grid_rowconfigure(1, weight=1)
labelframe1_lb2_4.grid_columnconfigure(1, weight=1)
labelframe1_lb2_4.grid_rowconfigure(2, weight=1)
labelframe1_lb2_4.grid_rowconfigure(3, weight=1)
labelframe1_lb2_4.grid_rowconfigure(4, weight=1)

labelframe1_lb3_4 = tkinter.LabelFrame(labelframe1_aba4, text="controle de velocidade", font=("Arial", 12, "bold"))
labelframe1_lb3_4.grid(sticky="news", row=2, column=0)
labelframe1_lb3_4.grid_rowconfigure(0, weight=1)
labelframe1_lb3_4.grid_columnconfigure(0, weight=1)
labelframe1_lb3_4.grid_rowconfigure(1, weight=1)
labelframe1_lb3_4.grid_columnconfigure(1, weight=1)
labelframe1_lb3_4.grid_rowconfigure(2, weight=1)
labelframe1_lb3_4.grid_rowconfigure(3, weight=1)
labelframe1_lb3_4.grid_rowconfigure(4, weight=1)

button_aumenta_tensao = tkinter.Button(labelframe1_lb3_4, text="aumenta 0.02 a tensao", command=aumenta_tensao)
button_aumenta_tensao.grid(row=2, column=0)

button_diminui_tensao = tkinter.Button(labelframe1_lb3_4, text="diminui 0.02 a tensao", command=diminui_tensao)
button_diminui_tensao.grid(row=4, column=0)

############# lbf2
labelframe2_aba4 = tkinter.LabelFrame(aba4, text="processamento", font=("Arial", 16, "bold"))
labelframe2_aba4.grid(sticky="news", row=0, column=1)
labelframe2_aba4.grid_rowconfigure(0, weight=1)
labelframe2_aba4.grid_columnconfigure(0, weight=1)
labelframe2_aba4.grid_rowconfigure(1, weight=1)
labelframe2_aba4.grid_rowconfigure(2, weight=1)
labelframe2_aba4.grid_rowconfigure(4, weight=1)

labelframe2_lb1_4 = tkinter.LabelFrame(labelframe2_aba4, text="tempo de aquisição", font=("Arial", 12, "bold"))
labelframe2_lb1_4.grid(sticky="news", row=0, column=0)
labelframe2_lb1_4.grid_rowconfigure(0, weight=1)
labelframe2_lb1_4.grid_columnconfigure(0, weight=1)
labelframe2_lb1_4.grid_rowconfigure(1, weight=1)
labelframe2_lb1_4.grid_columnconfigure(1, weight=1)
labelframe2_lb1_4.grid_rowconfigure(2, weight=1)
labelframe2_lb1_4.grid_columnconfigure(2, weight=1)
labelframe2_lb1_4.grid_rowconfigure(3, weight=1)
labelframe2_lb1_4.grid_rowconfigure(4, weight=1)

labelframe2_lb2_4 = tkinter.LabelFrame(labelframe2_aba4, text="decorrido ", font=("Arial", 12, "bold"))
labelframe2_lb2_4.grid(sticky="news", row=1, column=0)
labelframe2_lb2_4.grid_rowconfigure(0, weight=1)
labelframe2_lb2_4.grid_columnconfigure(0, weight=1)
labelframe2_lb2_4.grid_rowconfigure(1, weight=1)
labelframe2_lb2_4.grid_columnconfigure(1, weight=1)
labelframe2_lb2_4.grid_rowconfigure(2, weight=1)
labelframe2_lb2_4.grid_rowconfigure(3, weight=1)
labelframe2_lb2_4.grid_rowconfigure(4, weight=1)


# Dicionário para armazenar variáveis associadas a cada checkbox
checkboxchannels = {}

# Lista de opções para checkboxes
channelslistaabas = ["channel 0", "channel 1", "channel 2", "channel 3", "channel 4", "channel 5", "channel 6",
                     "channel 7", "channel 8"]

vars_globais = [
    canalativo0, canalativo1, canalativo2,
    canalativo3, canalativo4, canalativo5,
    canalativo6, canalativo7, canalativo8
]

# Cria e posiciona os checkboxes
for index, channelindex in enumerate(channelslistaabas):
    var = vars_globais[index] #tkinter.BooleanVar()  Use tk.IntVar() para armazenar 0 ou 1
    checkbox1 = tkinter.Checkbutton(labelframe2_lb2_4, text=channelindex, variable=var)    
    checkbox1.grid(row=index, column=0, sticky="w", padx=10, pady=5)
    checkboxchannels[channelindex] = var  # Armazena a variável associada

# Botão para mostrar os resultados
show_button = tkinter.Button(labelframe2_lb2_4, text="OK", command=get_channels)
show_button.grid(row=len(channels), column=0, padx=10, pady=10)


####canais
labelframe3_lb2_4 = tkinter.LabelFrame(labelframe2_aba4, text="eixo x do gráfico", font=("Arial", 12, "bold"))
labelframe3_lb2_4.grid(sticky="news", row=2, column=0)
labelframe3_lb2_4.grid_rowconfigure(0, weight=1)
labelframe3_lb2_4.grid_columnconfigure(0, weight=1)
labelframe3_lb2_4.grid_rowconfigure(1, weight=1)
labelframe3_lb2_4.grid_columnconfigure(1, weight=1)
labelframe3_lb2_4.grid_rowconfigure(2, weight=1)
labelframe3_lb2_4.grid_rowconfigure(3, weight=1)
labelframe3_lb2_4.grid_rowconfigure(4, weight=1)

aux_spinbox = tkinter.Spinbox(labelframe3_lb2_4, from_=0, to=100000)
aux_spinbox.insert(0, str(aux))
aux_spinbox.grid(row=2, column=1)
button_enter_aux = tkinter.Button(labelframe3_lb2_4, text="enviar", command=get_aux)
button_enter_aux.grid(row=3, column=1)

##################################
# Nicolas
# Criando o gráfico - Tudo abaixo, até ani3 é criação dos gráficos
# --- BARRA DE CONTROLE DE ESCALA (DENTRO DO FRAME DOS GRÁFICOS) ---
frame_toolbar = tkinter.Frame(frame_graficos, bg="#d9d9d9", height=40)
frame_toolbar.pack(side="top", fill="x", padx=5, pady=5)

# Rótulo
lbl_escala = tkinter.Label(frame_toolbar, text="Escala eixo X [min]:", font=("Arial", 9), bg="#d9d9d9")
lbl_escala.pack(side="left", padx=3, pady=3)

# Caixa de Entrada (Entry)
entry_escala_x = tkinter.Entry(frame_toolbar, width=6, font=("Arial", 9))
entry_escala_x.pack(side="left", padx=2)
entry_escala_x.insert(0, "0.83") # Valor inicial (equivale a 10000 amostras / 200hz / 60)

# Botão Atualizar
botao_atualizar_escala = tkinter.Button(frame_toolbar, text="Definir X", command=alterar_escala_tempo, font=("Arial", 9))
botao_atualizar_escala.pack(side="left", padx=5)

# Separador visual
tkinter.Label(frame_toolbar, text="|", bg="#d9d9d9", fg="gray").pack(side="left", padx=10)

# --- CONTROLE EIXO Y
btn_config_y = tkinter.Button(frame_toolbar, text="Configurar Eixos Y (Min / Max)", command=abrir_config_y, font=("Arial", 9))
btn_config_y.pack(side="left", padx=5)

fig1 = Figure(figsize=(6.5, 1.9), dpi=100)
ax1 = fig1.add_subplot(111)

# AJUSTE DE MARGENS GRAF. 1:
# top=0.92: Sobe o gráfico (perto do topo, pois não tem mais título)
# bottom=0.20: Dá espaço para o eixo X não cortar
fig1.subplots_adjust(top=0.92, bottom=0.20)

canvas1 = FigureCanvasTkAgg(fig1, master=frame_graficos)
canvas1.get_tk_widget().pack(side="top", fill="both", expand=True, padx=5, pady=2)

# Cria a Figura 2 (Inferior)
fig2 = Figure(figsize=(6.5, 2.3), dpi=100)
ax2 = fig2.add_subplot(111)
fig2.subplots_adjust(top=0.92, bottom=0.20)

canvas2 = FigureCanvasTkAgg(fig2, master=frame_graficos)
canvas2.get_tk_widget().pack(side="top", fill="both", expand=True, padx=5, pady=2)

fig3 = Figure(figsize=(6.5, 2.9), dpi=100)
ax3 = fig3.add_subplot(111)
fig3.subplots_adjust(top=0.90, bottom=0.35)
ax3_twin = None

canvas3 = FigureCanvasTkAgg(fig3, master=frame_graficos)
# side="top" ou "bottom" para empilhar os gráficos um embaixo do outro
canvas3.get_tk_widget().pack(side="top", fill="both", expand=True, padx=5, pady=2)


ani1 = FuncAnimation(fig1, update1, frames=np.linspace(0, 10, 100), interval=200, cache_frame_data="true")
ani2 = FuncAnimation(fig2, update2, frames=np.linspace(0, 10, 100), interval=200, cache_frame_data="true")
ani3 = FuncAnimation(fig3, update3, frames=np.linspace(0, 10, 100), interval=200, cache_frame_data="true")

#matplotlib.pyplot.show(block=False)

root.protocol("WM_DELETE_WINDOW", fechar_janela)

root.mainloop()

# nb.add(aba4, text = "channel 2")


""""
para_salvar = int(input("digite qual canal salvar:"))
print(f"{para_salvar}")

# salva o arquivo]
np.savetxt('valores.txt', allSamps[para_salvar])
"""












