"""
novo_tribometro.py
------------------
Interface grafica principal (Tkinter) do supervisorio do tribometro.

Objetivo geral:
- Coletar entradas do operador (ensaio, etapas, velocidades e parametros).
- Orquestrar ensaio usando executaveis externos (dlg_logger_ipc + a5_speed_logger).
- Exibir estado de execucao e graficos em tempo quase real durante o ensaio.

Arquitetura resumida:
- Camada de UI: widgets, validacoes de formulario e feedback visual.
- Camada de controle: start/pause/resume/stop, cronometro e estado da execucao.
- Camada de integracao: chamadas para orchestrator_runtime (processos e merge).

Modos presentes no arquivo:
- Versao atual: pipeline externo via orchestrator_runtime (USE_EXTERNAL_RUNNER=True).
- Fluxo da versao anterior: rotinas locais go/go_p mantidas para referencia tecnica.

Saidas de ensaio:
- Desktop\\Repositorio\\<AAAA-MM-DD - PoD - NomeEnsaio_Estudo-Repeticao>\\
  <data>-<nome_ensaio>-<estudo>-<repeticao>_I.csv,
  <data>-<nome_ensaio>-<estudo>-<repeticao>_DP.csv,
  <data>-<nome_ensaio>-<estudo>-<repeticao>_VP.csv,
  <data>-<nome_ensaio>-<estudo>-<repeticao>_T.csv,
  <data>-<nome_ensaio>-<estudo>-<repeticao>_G.png.
- Desktop\\Repositorio\\<AAAA-MM-DD - PoD - NomeEnsaio_Estudo-Repeticao>\\DadosDev\\
  <arquivo_t>.merge_source, dlg.csv, drive.csv, schedule.csv,
  graph_events.log, dlg_logger_events.log, a5_speed_events.log.

Pontos de atencao para manutencao:
- Evitar alterar protocolo IPC textual sem ajustar executaveis C.
- Manter taxa do DLG e do Drive alinhadas para sincronismo de linhas.
- Preservar validacoes de estado para nao permitir concorrencia de ensaios.

Resumo de funcoes chave:
- _load_app_settings/_save_app_settings: persistencia de configuracoes locais.
- check_status: check rapido de comunicacao com DLG e Drive.
- start_acquisition/pause_acquisition/stop_acquisition: controle de ensaio na UI.
- _wait_dlg_ok_and_start_timer/_tail_dlg_csv_for_graphs/_tail_turn_csv_for_graph3: sincronismo de inicio e graficos.
- update1/update2/update3: atualizacao periodica das curvas no matplotlib.
"""
# Bibliotecas padrao (Python):
# - os: caminhos, diretorios e operacoes de sistema de arquivos.
# - json: leitura/escrita de configuracoes persistidas em JSON.
# - time: relogio, sleeps e controle de temporizacao.
# - sys: acesso a dados do runtime (ex.: _MEIPASS no PyInstaller).
# - datetime: timestamps para nomeacao de pastas/arquivos de ensaio.
# - threading: tarefas em paralelo sem bloquear a UI Tkinter.
# - subprocess: execucao de processos externos (loggers C e utilitarios).
# - tempfile: arquivos temporarios para checks rapidos.
# - shutil: operacoes de alto nivel em arquivos/pastas (ex.: rmtree).
import os
import json
import time
import sys
import math
from datetime import datetime
import threading
import subprocess
import tempfile
import shutil

# UI (Tkinter):
# - tkinter: widgets base e janela principal.
# - messagebox: dialogs de alerta/erro/confirmacao.
# - filedialog: selecao de pastas/arquivos.
# - ttk: widgets tematicos (Notebook, etc.).
import tkinter
from tkinter import messagebox, filedialog
from tkinter import ttk

# Bibliotecas externas para calculo/plot:
# - numpy: suporte numerico para processamento e reducao de series.
# - matplotlib: graficos embutidos na UI Tkinter.
import numpy as np
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.figure import Figure

# Compatibilidade legada (Lynx/LDTP):
# - O fluxo principal atual usa executaveis C (dlg_logger_ipc + a5_speed_logger) via orchestrator_runtime.
# - Este bloco evita crash em PCs sem LDTP e mantem chamadas antigas (myModule.*) como no-op.
# - Remover somente quando nao houver mais referencias a LDTP_Const/myModule neste arquivo.
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

# orchestrator_runtime centraliza a orquestracao do pipeline externo:
# inicia/paralisa executaveis C, envia comandos IPC e realiza merge final.
import orchestrator_runtime as orch

# Execucao dos loggers C em background (DLG + Drive + merge)
# Switch de modo: True usa pipeline externo (executaveis C); False usa fluxo da versao anterior em Python.
# No geral a gente vai trabalhar com ele em true por que os outros executáveis (Programas C) que "coordenam" ensaios.
USE_EXTERNAL_RUNNER = True
# external_run_state guarda o RunState retornado pelo orchestrator:
# handles de processo, caminhos de CSV e parametros da execucao em curso.
external_run_state = None
# Token monotonic para invalidar threads antigas entre ensaios.
external_run_token = 0
# True enquanto o ensaio anterior ainda fecha processos, merge e arquivos finais.
run_finalizing = False
# Flag da rotina de tara para evitar execucoes concorrentes.
tara_running = False
# Estado do modo de monitoramento (DLG sem ensaio ativo).
monitor_mode_active = False
monitor_mode_token = 0
monitor_proc = None
monitor_tail_thread = None
monitor_csv_path = ""

# Persistencia simples de configuracoes do supervisório.
# Caminho padrao de saida caso o usuario ainda nao tenha configurado.
DEFAULT_REPO_BASE = os.path.join(os.path.expanduser("~"), "Desktop", "Repositorio")
# Relacao mecanica default (i) usada na conversao mm/s -> rpm.
DEFAULT_RELACAO = 1.0
# Tamanho padrao do intervalo de agregacao do grafico 3 (mm).
DEFAULT_INTERVALO_DIST_MM = 10.0
APP_SETTINGS_DIR = os.path.join(
    os.getenv("LOCALAPPDATA") or os.path.expanduser("~"),
    "LATRIB"
)
# Arquivo JSON de settings do usuario (persistencia local da UI).
APP_SETTINGS_PATH = os.path.join(APP_SETTINGS_DIR, "supervisorio_settings.json")


def _load_app_settings():
    """
    Carrega as configuracoes persistidas do supervisório.

    Fluxo:
    1) Tenta abrir APP_SETTINGS_PATH em modo leitura.
    2) Faz parse JSON para objeto Python.
    3) Aceita apenas dict; qualquer outro tipo vira {}.
    4) Em erro (arquivo ausente/corrompido/permissao), retorna {}.

    Returns:
        dict: dicionario de configuracoes validas ou vazio.
    """
    try:
        with open(APP_SETTINGS_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
            return data if isinstance(data, dict) else {}
    except Exception:
        return {}


def _save_app_settings(data):
    """
    Salva configuracoes do supervisório em JSON no APPDATA.

    Args:
        data (dict): mapa de configuracoes a persistir.

    Side effects:
        - Cria a pasta APP_SETTINGS_DIR se necessario.
        - Sobrescreve APP_SETTINGS_PATH.
        - Em caso de erro, suprime excecao para nao interromper UI.
    """
    try:
        os.makedirs(APP_SETTINGS_DIR, exist_ok=True)
        with open(APP_SETTINGS_PATH, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2, ensure_ascii=False)
        return True
    except Exception:
        return False


# APP_SETTINGS guarda o conteudo bruto carregado do JSON do usuario.
APP_SETTINGS = _load_app_settings()

# REPO_BASE define a pasta raiz de saida dos ensaios.
# - prioridade: valor salvo pelo usuario.
# - fallback: DEFAULT_REPO_BASE.
REPO_BASE = APP_SETTINGS.get("repo_base", DEFAULT_REPO_BASE)
if not isinstance(REPO_BASE, str) or not REPO_BASE.strip():
    REPO_BASE = DEFAULT_REPO_BASE

# RELACAO_MECANICA (i) e definida como D2 / D1.
# D1 = diametro da polia do motor; D2 = diametro da polia do disco.
# Conversao mm/s -> rpm_motor:
# rpm = (i * v * 60) / (2 * pi * raio_pino).
try:
    RELACAO_MECANICA = float(APP_SETTINGS.get("relacao", DEFAULT_RELACAO))
except Exception:
    RELACAO_MECANICA = DEFAULT_RELACAO
if RELACAO_MECANICA <= 0:
    RELACAO_MECANICA = DEFAULT_RELACAO

# Intervalo de distancia (mm) para agregacao do grafico 3 e arquivo _P.
try:
    DIST_INTERVAL_MM = float(APP_SETTINGS.get("dist_interval_mm", DEFAULT_INTERVALO_DIST_MM))
except Exception:
    DIST_INTERVAL_MM = DEFAULT_INTERVALO_DIST_MM
if DIST_INTERVAL_MM <= 0:
    DIST_INTERVAL_MM = DEFAULT_INTERVALO_DIST_MM


def _resource_path(name):
    """
    Encontra caminho de recursos em dois cenarios de execucao:
    - modo fonte (.py): usa pasta do arquivo atual (__file__).
    - modo PyInstaller one-file: usa sys._MEIPASS.

    Args:
        name (str): nome do recurso (ex.: 'logo.ico').

    Returns:
        str: caminho absoluto esperado para o recurso.
    """
    base_dir = getattr(sys, "_MEIPASS", os.path.dirname(__file__))
    return os.path.join(base_dir, name)


def _apply_app_icon(app_root):
    """
    Aplica icones da aplicacao na janela/barra de tarefas.

    Args:
        app_root (tkinter.Tk): janela principal da aplicacao.

    Side effects:
        - Tenta aplicar logo.ico via iconbitmap.
        - Tenta aplicar logo.png via iconphoto.
        - Guarda referencia em app_root._logo_img para evitar GC da imagem.
        - Em falha, ignora para nao travar a inicializacao da UI.
    """
    try:
        ico = _resource_path("logo.ico")
        if os.path.exists(ico):
            app_root.iconbitmap(ico)
    except Exception:
        pass

    try:
        png = _resource_path("logo.png")
        if os.path.exists(png):
            img = tkinter.PhotoImage(file=png)
            app_root._logo_img = img
            app_root.iconphoto(True, img)
    except Exception:
        pass


def _set_repo_base(path):
    """
    Atualiza REPO_BASE em memoria, persiste em JSON e sincroniza UI.

    Args:
        path (str): novo diretorio base para salvar ensaios.
    """
    global REPO_BASE, APP_SETTINGS
    if not path:
        return
    REPO_BASE = path
    APP_SETTINGS["repo_base"] = REPO_BASE
    saved_ok = _save_app_settings(APP_SETTINGS)
    if "repo_base_var" in globals():
        repo_base_var.set(REPO_BASE)
    log_msg(f"Diretorio base definido para: {REPO_BASE}")
    if not saved_ok:
        messagebox.showwarning(
            "Diretorio base",
            "Diretorio aplicado em memoria, mas houve falha ao salvar configuracao local."
        )


def _set_relacao_mecanica(valor):
    """
    Atualiza relacao mecanica global, persiste em JSON e sincroniza UI.

    Args:
        valor (float): valor positivo da relacao mecanica i.
    """
    global RELACAO_MECANICA, APP_SETTINGS
    RELACAO_MECANICA = valor
    APP_SETTINGS["relacao"] = RELACAO_MECANICA
    saved_ok = _save_app_settings(APP_SETTINGS)
    if "relacao_var" in globals():
        relacao_var.set(f"{RELACAO_MECANICA:.6g}")
    log_msg(f"Relacao mecanica definida para: {RELACAO_MECANICA:.6g}")
    return saved_ok


def _set_dist_interval_mm(valor):
    """
    Atualiza intervalo de distancia (mm), persiste em JSON e sincroniza UI.

    Args:
        valor (float): tamanho do intervalo em milimetros.
    """
    global DIST_INTERVAL_MM, APP_SETTINGS
    DIST_INTERVAL_MM = valor
    APP_SETTINGS["dist_interval_mm"] = DIST_INTERVAL_MM
    saved_ok = _save_app_settings(APP_SETTINGS)
    if "dist_interval_var" in globals():
        dist_interval_var.set(f"{DIST_INTERVAL_MM:.6g}")
    log_msg(f"Intervalo de distancia definido para: {DIST_INTERVAL_MM:.6g} mm")
    return saved_ok


def salvar_relacao_mecanica():
    """
    Handler da UI para validar e salvar o campo de relacao mecanica.

    Fluxo:
    1) Le texto do entry relacao_var.
    2) Normaliza virgula para ponto.
    3) Valida float > 0.
    4) Chama _set_relacao_mecanica().
    """
    txt = relacao_var.get().strip().replace(",", ".")
    if not txt:
        messagebox.showwarning("Relacao", "Informe um valor para a relacao.")
        return
    try:
        valor = float(txt)
    except Exception:
        messagebox.showwarning("Relacao", "Relacao invalida. Use numero maior que zero.")
        return
    if valor <= 0:
        messagebox.showwarning("Relacao", "Relacao deve ser maior que zero.")
        return
    saved_ok = _set_relacao_mecanica(valor)
    # Atualiza imediatamente os campos derivados na tabela.
    try:
        calcular_voltas_cursos_duracao()
    except Exception:
        pass
    if saved_ok:
        messagebox.showinfo("Relacao", f"Relacao mecanica salva: {valor:.6g}")
    else:
        messagebox.showwarning(
            "Relacao",
            "Relacao aplicada em memoria, mas houve falha ao salvar configuracao local."
        )


def salvar_intervalo_distancia():
    """
    Handler da UI para validar e salvar o intervalo do grafico 3 por distancia.
    """
    txt = dist_interval_var.get().strip().replace(",", ".")
    if not txt:
        messagebox.showwarning("Intervalo", "Informe um valor para o intervalo de distancia.")
        return
    try:
        valor = float(txt)
    except Exception:
        messagebox.showwarning("Intervalo", "Valor invalido. Use numero maior que zero.")
        return
    if valor <= 0:
        messagebox.showwarning("Intervalo", "Intervalo deve ser maior que zero.")
        return
    saved_ok = _set_dist_interval_mm(valor)
    if saved_ok:
        messagebox.showinfo("Intervalo", f"Intervalo de distancia salvo: {valor:.6g} mm")
    else:
        messagebox.showwarning(
            "Intervalo",
            "Intervalo aplicado em memoria, mas houve falha ao salvar configuracao local."
        )


def selecionar_repo_base():
    """
    Abre dialog de pasta e define REPO_BASE com validacoes basicas.

        - Mostra filedialog para usuario escolher pasta.
        - Garante existencia da pasta.
        - Em erro, mostra messagebox e nao altera configuracao.
    """
    initial_dir = REPO_BASE if os.path.isdir(REPO_BASE) else os.path.expanduser("~")
    path = filedialog.askdirectory(
        title="Selecionar diretorio base dos ensaios",
        initialdir=initial_dir
    )
    if not path:
        return
    try:
        os.makedirs(path, exist_ok=True)
    except Exception as e:
        messagebox.showerror("Erro", f"Nao foi possivel usar este diretorio.\n\n{e}")
        return
    _set_repo_base(path)


# Verifica se existe processo externo de DLG/Drive ainda rodando.
def _external_pipeline_ativo():
    """
    Verifica se algum processo do pipeline externo esta vivo.

    Returns:
        bool: True se dlg_proc ou drive_proc estiver em execucao.
    """
    if external_run_state is None:
        return False
    try:
        dlg_alive = external_run_state.dlg_proc and external_run_state.dlg_proc.poll() is None
        drv_alive = external_run_state.drive_proc and external_run_state.drive_proc.poll() is None
        return dlg_alive or drv_alive
    except Exception:
        return False


# Abre a UI de calibracao de canais, bloqueando quando ha ensaio/processos ativos.
def abrir_configurar_canais():
    """
    Abre CalibraDLG_UI.exe para configurar/capturar calibracao de canais.

    Regras:
    - Bloqueia abertura se ha ensaio ativo ou pipeline externo rodando.
    - Encontra caminho do executavel via orchestrator_runtime.
    - Abre processo em background via subprocess.Popen.
    """
    if _is_running() or _external_pipeline_ativo():
        messagebox.showwarning(
            "Configurar canais",
            "Finalize o ensaio e feche processos em segundo plano antes de calibrar."
        )
        return

    repo_root = orch.find_repo_root()
    exe = orch.find_calibra_ui_exe(repo_root)
    if not exe:
        messagebox.showerror(
            "Configurar canais",
            "CalibraDLG_UI.exe nao encontrado.\n\n"
            "Compile o CalibraDLG_UI antes de abrir esta tela."
        )
        return
    try:
        subprocess.Popen([exe], cwd=os.path.dirname(exe))
        log_msg("CalibraDLG_UI iniciado.")
    except Exception as e:
        messagebox.showerror("Configurar canais", f"Falha ao abrir CalibraDLG_UI.\n\n{e}")

# Log simples para a aba de debug (quando existir), removi ela nessa versão, mas é util enquanto desenvolvimento.
def log_msg(msg):
    """
    Registra mensagem no console e, se existir, no widget de log da UI.

    Args:
        msg (str): mensagem de status/erro para diagnostico.

    Side effects:
        - Imprime linha com timestamp no stdout.
        - Agenda append thread-safe no Tk via root.after.
    """
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


def graph_log(msg):
    """
    Log dedicado de confiabilidade dos graficos.

    - Replica no log principal (console/UI quando existir).
    - Grava em graph_events.log dentro da pasta do ensaio atual.
    """
    line = f"[GRAPH] {msg}"
    log_msg(line)
    if not graph_events_log_path:
        return
    try:
        ts = time.strftime('%H:%M:%S')
        with graph_events_lock:
            with open(graph_events_log_path, "a", encoding="utf-8") as f:
                f.write(f"[{ts}] {msg}\n")
    except Exception:
        pass


def _turn_rt_reset_state(initial_mode="fallback"):
    """
    Reseta o arbitro de fontes do grafico 3 para um novo ensaio.
    """
    global turn_rt_mode, turn_rt_stream_has_data, turn_rt_stream_last_ts, turn_rt_stream_closed
    mode = "fallback" if str(initial_mode).lower() == "fallback" else "stream"
    with turn_rt_lock:
        turn_rt_mode = mode
        turn_rt_stream_has_data = False
        turn_rt_stream_last_ts = time.time()
        # Se comecar em fallback, considera stream fechado para nao ficar
        # aguardando sinais de um processo inexistente.
        turn_rt_stream_closed = (mode == "fallback")


def _turn_rt_mark_stream_data():
    """
    Marca que o stream TURN recebeu dado neste instante.
    """
    global turn_rt_stream_has_data, turn_rt_stream_last_ts
    with turn_rt_lock:
        turn_rt_stream_has_data = True
        turn_rt_stream_last_ts = time.time()


def _turn_rt_mark_stream_closed():
    """
    Marca encerramento do stream TURN.
    """
    global turn_rt_stream_closed
    with turn_rt_lock:
        turn_rt_stream_closed = True


def _turn_rt_get_mode():
    with turn_rt_lock:
        return turn_rt_mode


def _turn_rt_switch_to_fallback(reason):
    """
    Faz failover de fonte do grafico 3 para agregacao local.
    """
    global turn_rt_mode
    do_log = False
    with turn_rt_lock:
        if turn_rt_mode != "fallback":
            turn_rt_mode = "fallback"
            do_log = True
    if do_log:
        graph_log(f"TURN RT fallback ativo: {reason}")


# Aviso unico se o modulo LDTP nao estiver disponivel.
if not HAVE_LDTP:
    log_msg(f"Aviso: LDTP_Const/myModule nao encontrados; controle analogico interno desativado ({_LDTP_IMPORT_ERROR}).")

# Atualiza um label de status para OK/X com cor correspondente.
def _set_status(label, ok):
    """
    Atualiza um indicador visual simples de status (OK/X).

    Args:
        label (tkinter.Label | None): label alvo da UI.
        ok (bool): True para estado saudavel, False para falha.
    """
    if label is None:
        return
    if ok:
        label.config(text='OK', fg='green')
    else:
        label.config(text='X', fg='red')


def _set_start_button_state():
    """
    Atualiza estado do botao Iniciar conforme modo monitoramento.

    Regras:
    - Monitoramento ativo: impede inicio de ensaio para evitar concorrencia.
    - Monitoramento inativo: restaura botao.
    """
    if "button_frame4_iniciar" not in globals():
        return
    try:
        disabled = (
            monitor_mode_active
            or tara_running
            or run_finalizing
            or _is_running()
            or _external_pipeline_ativo()
        )
        button_frame4_iniciar.config(state=("disabled" if disabled else "normal"))
    except Exception:
        pass


def _set_run_finalizing(value):
    """
    Atualiza o bloqueio de inicio durante fechamento/merge do ensaio anterior.
    """
    global run_finalizing
    run_finalizing = bool(value)
    try:
        _set_start_button_state()
    except Exception:
        pass


def _set_tara_running(value):
    """
    Atualiza o bloqueio visual/logico enquanto uma tara esta em andamento.
    """
    global tara_running
    tara_running = bool(value)
    try:
        if "button_frame7_zerar" in globals():
            button_frame7_zerar.config(state=("disabled" if tara_running else "normal"))
    except Exception:
        pass
    try:
        _set_start_button_state()
    except Exception:
        pass


# Monta parametros de subprocesso no Windows para evitar console piscando.
# Honestamente, essa parte eu pedi pro chatGPT remover o console que estava abrindo em alguns momentos
# Resolveu então vai ficar kkkkk
def _subprocess_no_window_kwargs():
    """
    Retorna kwargs para subprocess no Windows sem janela de console.

    Returns:
        dict: argumentos opcionais para subprocess.run/Popen.

    Contexto:
        O supervisório e uma app Tkinter; sem essas flags, cada utilitario
        C pode abrir uma janela de console visivel para o usuario.
    """
    kwargs = {}
    if os.name == 'nt':
        creationflags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
        kwargs['creationflags'] = creationflags
        if hasattr(subprocess, 'STARTUPINFO') and hasattr(subprocess, 'STARTF_USESHOWWINDOW'):
            startupinfo = subprocess.STARTUPINFO()
            startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            kwargs['startupinfo'] = startupinfo
    return kwargs


# Executa um check curto do DLG via logger IPC e retorna resultado/detalhe.
# Boa parte das funcoes a baixo estão relacionadas a rotina de check status que foi adicionada
# Mas sao bons exemplos da comunicacao Programa -> DLG
def _dlg_check_once(dlg_exe, dlg_ip, dlg_port, bind_port):
    """
    Executa um teste curto de aquisicao DLG via dlg_logger_ipc em modo IPC.

    Args:
        dlg_exe (str): caminho do executavel dlg_logger_ipc.exe.
        dlg_ip (str): IP do equipamento DLG.
        dlg_port (int): porta UDP do DLG (tipicamente 41401).
        bind_port (int): porta local de bind para receber pacotes.

    Returns:
        tuple[bool, str]:
            - ok=True quando houver evidencias de dados validos (DATA_OK ou err=0 em CSV).
            - detalhe com motivo de falha quando aplicavel.

    Protocolo usado:
        - Processo e iniciado com --ipc.
        - Este codigo envia "START\\n" no stdin do processo.
        - O logger responde no stdout com marcadores como DATA_OK/DATA_TIMEOUT.
    """
    with tempfile.NamedTemporaryFile(delete=False, suffix='.csv') as tmp:
        tmp_path = tmp.name

    cmd = [
        dlg_exe,
        '--out', tmp_path,
        '--duration', '2',
        '--rate', f"{getattr(orch, 'DEFAULT_RATE_HZ', 50.0):.0f}",
        '--ip', str(dlg_ip),
        '--port', str(dlg_port),
        '--bind-port', str(bind_port),
        '--ipc',
    ]
    # cmd acima inicia um logger headless de curta duracao:
    # - escreve CSV temporario em tmp_path
    # - fica aguardando START no stdin por estar em modo --ipc
    # - tenta receber ACQDATA do DLG na porta bind_port

    detail = ''
    ok = False
    try:
        res = subprocess.run(
            cmd,
            input='START\n',
            capture_output=True,
            text=True,
            timeout=10,
            **_subprocess_no_window_kwargs()
        )
        # Analise de resposta:
        # - stdout/stderr podem conter DATA_OK ou DATA_TIMEOUT.
        # - returncode != 0 normalmente indica erro operacional do logger.
        out_txt = ((res.stdout or '') + '\n' + (res.stderr or '')).upper()
        if 'DATA_OK' in out_txt:
            ok = True
        elif 'DATA_TIMEOUT' in out_txt:
            detail = 'timeout sem ACQDATA'
        elif 'FALHA AO ABRIR UDP' in out_txt:
            detail = 'falha ao abrir UDP/bind'
        elif res.returncode != 0:
            detail = f'rc={res.returncode}'
    except subprocess.TimeoutExpired:
        detail = 'timeout executando logger'
    except Exception as e:
        detail = f'erro ({e})'

    # Fallback por CSV: considera valido se existir ao menos uma linha com err=0.
    if not ok:
        try:
            with open(tmp_path, 'r', encoding='utf-8') as f:
                next(f, None)
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    parts = line.split(',')
                    if parts and parts[-1].strip() == '0':
                        ok = True
                        break
        except Exception:
            pass

    try:
        os.remove(tmp_path)
    except Exception:
        pass

    return ok, detail


def _resolve_ch1_calib_path(repo_root, dlg_exe):
    """
    Resolve o caminho do arquivo de calibracao do CH1.

    Prioridade:
    Segue a mesma estrategia de busca do dlg_logger_ipc (DLG4000):
    - calib.json / calib / calib_CH1.json / out\\calib_CH1.json
    - os mesmos nomes na pasta do executavel do logger.
    """
    path_ch = "calib_CH1.json"
    path_out_ch = os.path.join("out", "calib_CH1.json")

    def _load_json_quick(path):
        try:
            with open(path, "r", encoding="utf-8") as f:
                return json.load(f)
        except Exception:
            return None

    # 1) Prioriza sempre arquivo explicito de CH1.
    explicit_candidates = [path_ch, path_out_ch]
    generic_candidates = ["calib.json", "calib"]

    if dlg_exe:
        exe_dir = os.path.dirname(dlg_exe)
        explicit_candidates.append(os.path.join(exe_dir, path_ch))
        explicit_candidates.append(os.path.join(exe_dir, path_out_ch))
        generic_candidates.append(os.path.join(exe_dir, "calib.json"))
        generic_candidates.append(os.path.join(exe_dir, "calib"))

    # Fallback explicito para o mesmo diretorio usado por check_executables().
    if repo_root:
        release_dir = os.path.join(repo_root, "DLG4000", "bin", "Release")
        explicit_candidates.append(os.path.join(release_dir, "calib_CH1.json"))
        explicit_candidates.append(os.path.join(release_dir, "out", "calib_CH1.json"))
        generic_candidates.append(os.path.join(release_dir, "calib.json"))
        generic_candidates.append(os.path.join(release_dir, "calib"))

    for path in explicit_candidates:
        if path and os.path.exists(path):
            return path

    # 2) Em arquivo generico, aceita somente quando houver indicio de CH1.
    for path in generic_candidates:
        if not path or not os.path.exists(path):
            continue
        calib = _load_json_quick(path)
        if not isinstance(calib, dict):
            continue
        if _is_ch1_calib_payload(calib, path):
            return path
    return None


def _is_ch1_calib_payload(calib, calib_path):
    """
    Valida se um payload de calibracao pertence ao CH1.

    Regras:
    - Nome do arquivo contendo calib_CH1.
    - Campo channel igual a 1 ou CH1.
    - Lista channels contendo 1.
    - Em arquivo sem marcador explicito de canal, aceita por compatibilidade.
    """
    if not isinstance(calib, dict):
        return False

    is_ch1 = False
    base_name = os.path.basename(calib_path).lower() if calib_path else ""
    if "calib_ch1" in base_name:
        is_ch1 = True

    ch = calib.get("channel")
    if not is_ch1 and ch is not None:
        try:
            if int(ch) == 1:
                is_ch1 = True
        except Exception:
            try:
                if str(ch).strip().upper() == "CH1":
                    is_ch1 = True
            except Exception:
                pass

    ch_list = calib.get("channels")
    if not is_ch1 and isinstance(ch_list, list):
        for item in ch_list:
            try:
                if int(item) == 1:
                    is_ch1 = True
                    break
            except Exception:
                pass

    # Se houver marcador explicito e nao for CH1, rejeita.
    if not is_ch1 and (ch is not None or isinstance(ch_list, list)):
        return False

    return True


def _load_ch1_fit_data(repo_root, dlg_exe):
    """
    Carrega os parametros de ajuste (fit) da calibracao CH1.

    Retorno:
        dict com keys: path, slope, intercept, r2
        ou None se nao existir/nao for possivel ler.
    """
    calib_path = _resolve_ch1_calib_path(repo_root, dlg_exe)
    if not calib_path:
        return None

    try:
        with open(calib_path, "r", encoding="utf-8") as f:
            calib = json.load(f)
        fit = calib.get("fit", {})

        # Se o arquivo identificar explicitamente outro canal, ignora.
        if not _is_ch1_calib_payload(calib, calib_path):
            return None

        return {
            "path": calib_path,
            "slope": float(fit.get("slope")),
            "intercept": float(fit.get("intercept")),
            "r2": float(fit.get("r2")),
        }
    except Exception:
        return None


def _capture_ch1_mean_tara_once(dlg_exe, dlg_ip, dlg_port, bind_port, duration_s=30, rate_hz=50.0):
    """
    Executa uma captura curta do DLG e retorna media valida do CH1.

    Retorna:
        tuple[float, int]: media_ch1, n_validas
    """
    with tempfile.NamedTemporaryFile(delete=False, suffix=".csv") as tmp:
        tmp_path = tmp.name

    cmd = [
        dlg_exe,
        "--out", tmp_path,
        "--duration", str(int(duration_s)),
        "--rate", f"{float(rate_hz):.0f}",
        "--ip", str(dlg_ip),
        "--port", str(dlg_port),
        "--bind-port", str(bind_port),
        "--ipc",
    ]

    try:
        # Tempo de sobra para setup/flush alem da janela de 30 s.
        timeout_s = max(40, int(duration_s) + 15)
        res = subprocess.run(
            cmd,
            input="START\n",
            capture_output=True,
            text=True,
            timeout=timeout_s,
            **_subprocess_no_window_kwargs()
        )
        if res.returncode != 0:
            log_msg(f"Tara CH1: dlg_logger_ipc retornou rc={res.returncode} (tentando ler CSV).")

        total = 0.0
        count = 0
        with open(tmp_path, "r", encoding="utf-8") as f:
            next(f, None)
            for line in f:
                line = line.strip()
                if not line:
                    continue
                parts = line.split(",")
                # Formato esperado: idx,t_qpc,t_s,ch1..ch8,atrito,err
                if len(parts) < 13:
                    continue
                err_txt = parts[-1].strip()
                ch1_txt = parts[3].strip()
                if err_txt != "0":
                    continue
                if not ch1_txt or ch1_txt.upper() == "NULL":
                    continue
                try:
                    v = float(ch1_txt)
                except Exception:
                    continue
                total += v
                count += 1

        if count <= 0:
            raise RuntimeError("captura sem amostras validas de CH1")

        return (total / count), count
    finally:
        try:
            os.remove(tmp_path)
        except Exception:
            pass


def _capture_ch1_mean_tara(dlg_exe, dlg_ip, dlg_port, duration_s=30, rate_hz=50.0):
    """
    Captura media de CH1 para tara, com fallback de bind.
    """
    last_err = None
    for bind_port in [41402, 0]:
        try:
            mean_v, n_valid = _capture_ch1_mean_tara_once(
                dlg_exe=dlg_exe,
                dlg_ip=dlg_ip,
                dlg_port=dlg_port,
                bind_port=bind_port,
                duration_s=duration_s,
                rate_hz=rate_hz,
            )
            return mean_v, n_valid, bind_port
        except Exception as e:
            last_err = e
            if bind_port == 41402:
                log_msg("Tara CH1: bind 41402 falhou; tentando porta efemera.")
    raise last_err if last_err else RuntimeError("falha desconhecida na captura de tara")


def _write_zero_debug_csv(csv_path, report):
    """
    Registra resultado da rotina de zeramento em CSV (modo append).

    O arquivo eh usado para auditoria e suporte tecnico, mantendo historico
    de intercept/slope antes e depois de cada tara.
    """
    if not csv_path:
        return
    os.makedirs(os.path.dirname(csv_path), exist_ok=True)
    need_header = not os.path.exists(csv_path)
    with open(csv_path, "a", encoding="utf-8", newline="\n") as f:
        if need_header:
            f.write(
                "timestamp;modo;status;calib_path;bind_port;n_validas;media_ch1;"
                "slope_antes;slope_depois;intercept_antes;intercept_depois;"
                "delta_slope;delta_intercept;erro\n"
            )
        f.write(
            f"{report.get('timestamp','')};"
            f"{report.get('mode','')};"
            f"{report.get('status','')};"
            f"{report.get('calib_path','')};"
            f"{report.get('bind_port','')};"
            f"{report.get('n_validas','')};"
            f"{report.get('media_ch1','')};"
            f"{report.get('slope_before','')};"
            f"{report.get('slope_after','')};"
            f"{report.get('intercept_before','')};"
            f"{report.get('intercept_after','')};"
            f"{report.get('delta_slope','')};"
            f"{report.get('delta_intercept','')};"
            f"{report.get('error','')}\n"
        )


def _build_zero_avulso_log_path():
    """
    Define o caminho de log da tara avulsa.

    Estrutura:
      <REPO_BASE>\\ZeroAvulso\\zero_avulso_<timestamp>.csv
    """
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    base_dir = os.path.join(REPO_BASE, "ZeroAvulso")
    os.makedirs(base_dir, exist_ok=True)
    return os.path.join(base_dir, f"zero_avulso_{ts}.csv")


def _run_ch1_tara_core(repo_root, dlg_exe, calib_path, duration_s=30, rate_hz=50.0):
    """
    Executa a tara do CH1 e retorna um relatorio detalhado.

    Regras:
    - Ajusta somente `fit.intercept`.
    - Preserva `fit.slope` de forma explicita.
    - Reabre o arquivo apos gravacao para confirmar valores aplicados.
    """
    dlg_ip = getattr(orch, "DEFAULT_DLG_IP", "192.168.1.100")
    dlg_port = getattr(orch, "DEFAULT_DLG_PORT", 41401)
    mean_ch1, n_valid, bind_used = _capture_ch1_mean_tara(
        dlg_exe=dlg_exe,
        dlg_ip=dlg_ip,
        dlg_port=dlg_port,
        duration_s=duration_s,
        rate_hz=rate_hz,
    )

    with open(calib_path, "r", encoding="utf-8") as f:
        calib = json.load(f)
    fit = calib.get("fit")
    if not isinstance(fit, dict):
        raise RuntimeError("arquivo de calibracao sem bloco fit")

    slope_before = float(fit.get("slope", 1.0))
    intercept_before = float(fit.get("intercept", 0.0))

    # Tara: remove o offset medio medido no repouso.
    intercept_after = intercept_before - mean_ch1
    fit["intercept"] = intercept_after
    # Guarda de consistencia: slope nao deve ser alterado na tara.
    fit["slope"] = slope_before

    calib_tmp = calib_path + ".tmp"
    with open(calib_tmp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(calib, f, indent=2, ensure_ascii=False)
        f.write("\n")
    os.replace(calib_tmp, calib_path)

    with open(calib_path, "r", encoding="utf-8") as f:
        calib_after = json.load(f)
    fit_after = calib_after.get("fit", {})
    slope_after = float(fit_after.get("slope", slope_before))
    intercept_after_file = float(fit_after.get("intercept", intercept_after))

    if abs(slope_after - slope_before) > 1e-12:
        raise RuntimeError(
            "Slope foi alterado durante a tara (esperado: sem alteracao)."
        )

    return {
        "timestamp": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "calib_path": calib_path,
        "bind_port": bind_used,
        "n_validas": n_valid,
        "media_ch1": f"{mean_ch1:.10g}",
        "slope_before": f"{slope_before:.12g}",
        "slope_after": f"{slope_after:.12g}",
        "intercept_before": f"{intercept_before:.12g}",
        "intercept_after": f"{intercept_after_file:.12g}",
        "delta_slope": f"{(slope_after - slope_before):.12g}",
        "delta_intercept": f"{(intercept_after_file - intercept_before):.12g}",
    }


def _ask_ready_step_dialog(title, body_text, ok_text):
    """
    Exibe dialogo modal com botao de confirmacao customizado.
    """
    result = {"ok": False}
    dlg = tkinter.Toplevel(root)
    dlg.title(title)
    dlg.resizable(False, False)
    dlg.transient(root)
    dlg.grab_set()

    frm = tkinter.Frame(dlg, padx=16, pady=12)
    frm.pack(fill="both", expand=True)
    lbl = tkinter.Label(frm, text=body_text, justify="left", anchor="w")
    lbl.pack(fill="x", pady=(0, 12))

    btns = tkinter.Frame(frm)
    btns.pack(fill="x")

    def _ok():
        result["ok"] = True
        dlg.destroy()

    def _cancel():
        result["ok"] = False
        dlg.destroy()

    tkinter.Button(btns, text=ok_text, width=24, command=_ok).pack(side="left")
    tkinter.Button(btns, text="Cancelar", width=12, command=_cancel).pack(side="right")

    dlg.protocol("WM_DELETE_WINDOW", _cancel)
    dlg.update_idletasks()
    try:
        x = root.winfo_rootx() + max(20, (root.winfo_width() - dlg.winfo_width()) // 2)
        y = root.winfo_rooty() + max(20, (root.winfo_height() - dlg.winfo_height()) // 2)
        dlg.geometry(f"+{x}+{y}")
    except Exception:
        pass
    root.wait_window(dlg)
    return result["ok"]


def _cleanup_run_folder_on_abort():
    """
    Remove a pasta criada para o ensaio atual quando o inicio e cancelado.
    """
    global caminho_arquivo_1, caminho_arquivo_2, caminho_pasta
    global caminho_dlg_csv, caminho_drive_csv, caminho_turn_dist_csv, caminho_turn_volta_csv, caminho_merge_csv, caminho_schedule_csv, caminho_graficos_png
    global graph_events_log_path, run_finalizing
    try:
        if caminho_pasta and os.path.exists(caminho_pasta):
            shutil.rmtree(caminho_pasta, ignore_errors=True)
    except Exception:
        pass
    caminho_arquivo_1 = ""
    caminho_arquivo_2 = ""
    caminho_dlg_csv = ""
    caminho_drive_csv = ""
    caminho_turn_dist_csv = ""
    caminho_turn_volta_csv = ""
    caminho_merge_csv = ""
    caminho_schedule_csv = ""
    caminho_graficos_png = ""
    graph_events_log_path = ""
    run_finalizing = False
    try:
        _set_start_button_state()
    except Exception:
        pass


def _run_auto_tara_before_start(repo_root, exe_info):
    """
    Executa fluxo guiado de tara antes de iniciar cada ensaio.

    Passos:
    1) Instrui operador a deixar sistema sem carga.
    2) Captura tara (30 s) e ajusta intercept do CH1.
    3) Instrui operador a recolocar carga e preparar inicio do ensaio.
    """
    if tara_running:
        messagebox.showwarning("Tara em andamento", "Aguarde a tara atual terminar antes de iniciar outro ensaio.")
        return False

    dlg_exe = exe_info.get("dlg_exe")
    if not dlg_exe:
        messagebox.showerror(
            "Calibracao obrigatoria",
            "dlg_logger_ipc.exe nao encontrado.\n\nNao e possivel iniciar sem zerar a celula."
        )
        return False

    calib_path = _resolve_ch1_calib_path(repo_root, dlg_exe)
    if not calib_path:
        messagebox.showerror(
            "Calibracao obrigatoria",
            "Arquivo de calibracao CH1 nao encontrado (calib_CH1.json).\n\n"
            "Nao e possivel iniciar sem zerar a celula."
        )
        return False

    prev_text = label_ensaio_estado.cget("text")
    prev_fg = label_ensaio_estado.cget("fg")
    _set_tara_running(True)
    label_ensaio_estado.config(text="Preparando tara automatica", fg="#0078D4")
    root.update_idletasks()

    messagebox.showinfo(
        "Calibracao antes do ensaio",
        "Antes de iniciar, sera executado o zeramento automatico da celula de carga."
    )
    if not _ask_ready_step_dialog(
        "Preparacao para zeramento",
        "Remova a carga e deixe o braco do pino em equilibrio,\nsem contato com o disco.",
        "Em condicoes de zerar",
    ):
        label_ensaio_estado.config(text=prev_text, fg=prev_fg)
        _set_tara_running(False)
        return False

    log_report_path = os.path.join(caminho_pasta, "DadosDev", "zero_ensaio.csv")
    report = {
        "timestamp": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "mode": "auto_ensaio",
    }

    try:
        label_ensaio_estado.config(text="Coletando dados para tara", fg="#0078D4")
        root.update()
        rate_hz = getattr(orch, "DEFAULT_RATE_HZ", 50.0)
        report.update(
            _run_ch1_tara_core(
                repo_root=repo_root,
                dlg_exe=dlg_exe,
                calib_path=calib_path,
                duration_s=30,
                rate_hz=rate_hz,
            )
        )
        report["status"] = "OK"
        report["error"] = ""
        _write_zero_debug_csv(log_report_path, report)
        log_msg(
            "Tara auto: ok (path={0}, slope {1} -> {2}, intercept {3} -> {4}).".format(
                report.get("calib_path", ""),
                report.get("slope_before", ""),
                report.get("slope_after", ""),
                report.get("intercept_before", ""),
                report.get("intercept_after", ""),
            )
        )
    except Exception as e:
        report["status"] = "FAIL"
        report["error"] = str(e).replace("\n", " ").replace(";", ",")
        _write_zero_debug_csv(log_report_path, report)
        label_ensaio_estado.config(text=prev_text, fg=prev_fg)
        _set_tara_running(False)
        messagebox.showerror("Calibracao obrigatoria", f"Falha no zeramento automatico.\n\n{e}")
        return False

    label_ensaio_estado.config(text="Aguardando condicao para iniciar ensaio", fg="#0078D4")
    root.update_idletasks()
    if not _ask_ready_step_dialog(
        "Preparacao final",
        "Coloque a carga e posicione o pino sobre o disco.",
        "Em condicoes de iniciar ensaio",
    ):
        label_ensaio_estado.config(text=prev_text, fg=prev_fg)
        _set_tara_running(False)
        return False

    label_ensaio_estado.config(text=prev_text, fg=prev_fg)
    _set_tara_running(False)
    return True


def zerar_celula():
    """
    Executa tara de CH1:
    1) coleta CH1 por 30 s com o sistema em repouso;
    2) calcula media das amostras validas;
    3) ajusta intercept da calibracao CH1 para deslocar o zero.
    """
    global tara_running

    if tara_running:
        messagebox.showwarning("Zerar celula", "Rotina de tara ja esta em execucao.")
        return

    if _is_running() or _external_pipeline_ativo():
        messagebox.showwarning(
            "Zerar celula",
            "Finalize o ensaio/processos em segundo plano antes de executar a tara."
        )
        return

    repo_root = orch.find_repo_root()
    exe_info = orch.check_executables(repo_root)
    dlg_exe = exe_info.get("dlg_exe")
    if not dlg_exe:
        messagebox.showerror(
            "Zerar celula",
            "dlg_logger_ipc.exe nao encontrado.\n\nCompile os executaveis C antes de rodar a tara."
        )
        return

    calib_path = _resolve_ch1_calib_path(repo_root, dlg_exe)
    if not calib_path:
        messagebox.showerror(
            "Zerar celula",
            "Arquivo de calibracao CH1 nao encontrado (calib_CH1.json)."
        )
        return

    prev_text = label_ensaio_estado.cget("text")
    prev_fg = label_ensaio_estado.cget("fg")
    _set_tara_running(True)
    label_ensaio_estado.config(text="Coletando dados para tara", fg="#0078D4")
    log_msg("Tara CH1: iniciando coleta de 30 s.")

    def _worker():
        ok = False
        out_msg = ""
        report = {
            "timestamp": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            "mode": "manual_avulso",
        }
        zero_log_path = _build_zero_avulso_log_path()
        try:
            rate_hz = getattr(orch, "DEFAULT_RATE_HZ", 50.0)
            report.update(
                _run_ch1_tara_core(
                    repo_root=repo_root,
                    dlg_exe=dlg_exe,
                    calib_path=calib_path,
                    duration_s=30,
                    rate_hz=rate_hz,
                )
            )
            ok = True
            report["status"] = "OK"
            report["error"] = ""
            _write_zero_debug_csv(zero_log_path, report)
            out_msg = (
                f"Tara aplicada com sucesso.\n\n"
                f"Arquivo: {report.get('calib_path','')}\n"
                f"Bind usado: {report.get('bind_port','')}\n"
                f"Amostras validas CH1: {report.get('n_validas','')}\n"
                f"Media CH1 (30 s): {report.get('media_ch1','')}\n"
                f"Slope antigo: {report.get('slope_before','')}\n"
                f"Slope novo: {report.get('slope_after','')}\n"
                f"Intercept antigo: {report.get('intercept_before','')}\n"
                f"Intercept novo: {report.get('intercept_after','')}\n\n"
                f"Log avulso: {zero_log_path}"
            )
            log_msg(
                "Tara CH1 manual: ok (path={0}, slope {1} -> {2}, intercept {3} -> {4}).".format(
                    report.get("calib_path", ""),
                    report.get("slope_before", ""),
                    report.get("slope_after", ""),
                    report.get("intercept_before", ""),
                    report.get("intercept_after", ""),
                )
            )
        except Exception as e:
            report["status"] = "FAIL"
            report["error"] = str(e).replace("\n", " ").replace(";", ",")
            _write_zero_debug_csv(zero_log_path, report)
            out_msg = f"Falha ao executar tara da celula.\n\n{e}"
            log_msg(f"Tara CH1: falha ({e}).")

        def _finish_ui():
            _set_tara_running(False)
            if not _is_running():
                label_ensaio_estado.config(text=prev_text, fg=prev_fg)
            if ok:
                messagebox.showinfo("Zerar celula", out_msg)
            else:
                messagebox.showerror("Zerar celula", out_msg)

        root.after(0, _finish_ui)

    threading.Thread(target=_worker, daemon=True).start()


# Check simples de comunicacao (DLG + Drive) acionado pelo botao de status.
def check_status():
    """
    Valida conectividade operacional com os dois lados de hardware.

    Fluxo:
    1) DLG: executa dlg_logger_ipc por ~2 s em IPC e busca DATA_OK.
    2) Drive: executa a5_pos_cli --diag na COM5.
    3) Atualiza labels visuais de status (DLG/Drive).

    Observacao:
        Este check verifica comunicacao de aplicacao/protocolo.
    """
    log_msg('Check status: iniciando.')

    dlg_ok = False
    drive_ok = False

    repo_root = orch.find_repo_root()

    # ---- DLG ----
    # Este bloco valida "na pratica" se existe comunicacao com o DLG:
    # em vez de ping ICMP, ele sobe o logger IPC por poucos segundos e
    # verifica se chega ao menos uma amostra ACQDATA valida.
    try:
        # check_executables() retorna um dicionario com caminhos dos .exe
        # que o supervisorio precisa (dlg_logger_ipc, a5_speed_logger etc.).
        exe_info = orch.check_executables(repo_root)
        # Pegamos somente o executavel do logger DLG para o check atual.
        dlg_exe = exe_info.get('dlg_exe')
        if not dlg_exe:
            # Sem executavel nao existe como testar protocolo/acquisicao.
            # Aqui nao e erro fatal da UI; apenas registramos o diagnostico.
            log_msg('DLG check: dlg_logger_ipc.exe nao encontrado.')
        else:
            # Usa constantes do orchestrator quando disponiveis.
            # getattr com fallback evita quebra se a constante nao existir
            # (mantem compatibilidade com versoes antigas do modulo).
            dlg_ip = getattr(orch, 'DEFAULT_DLG_IP', '192.168.1.100')
            dlg_port = getattr(orch, 'DEFAULT_DLG_PORT', 41401)
            log_msg('DLG check: executando dlg_logger_ipc (ipc, 2s)...')
            # Primeiro tenta a mesma porta de bind do logger real (41402).
            # Se falhar (porta ocupada), tenta porta efemera.
            # - 41402: replica o cenario normal de execucao do pipeline.
            # - 0: pede ao SO uma porta livre qualquer (fallback robusto).
            attempts = [41402, 0]
            # detail carrega o motivo da ultima falha retornado por
            # _dlg_check_once (ex.: timeout sem ACQDATA, falha de bind UDP).
            detail = ''
            for bind_port in attempts:
                # _dlg_check_once executa um mini teste:
                # 1) sobe dlg_logger_ipc em --ipc
                # 2) envia START via stdin (message passing)
                # 3) tenta receber dados do DLG por ~2 s
                # 4) retorna (ok, detail)
                # Parametros:
                # - dlg_exe: caminho do executavel a testar
                # - dlg_ip/dlg_port: destino UDP do equipamento DLG
                # - bind_port: porta local para receber ACQDATA
                dlg_ok, detail = _dlg_check_once(dlg_exe, dlg_ip, dlg_port, bind_port)
                if dlg_ok:
                    # Achou amostra valida: nao precisa novas tentativas.
                    break
                if bind_port == 41402:
                    # Log explicito para mostrar que o fallback foi acionado.
                    log_msg('DLG check: tentativa em 41402 falhou; tentando porta efemera...')

            if dlg_ok:
                # dlg_ok=True significa "recebeu dado valido no mini teste".
                log_msg('DLG check: ok.')
            else:
                # Sem amostra valida, priorizamos mensagem com causa tecnica.
                if detail:
                    log_msg(f'DLG check: nenhuma amostra valida ({detail}).')
                else:
                    # Caso raro: falha sem detalhe textual especifico.
                    log_msg('DLG check: nenhuma amostra valida.')
    except Exception as e:
        # Ultima defesa: erros inesperados do check nao podem derrubar a tela.
        log_msg(f'DLG check: erro ({e}).')
   
    # ---- Drive ----
    # Este bloco valida comunicacao serial/Modbus com o DriveA5.
    # O objetivo e confirmar que o CLI consegue abrir a porta e ler
    try:
        # Preferir a5_pos_cli --diag:
        # - comando curto de diagnostico
        # - evita logica de ensaio longo
        # - menor risco de alterar estado operacional
        # candidates lista caminhos comuns de build em ordem de preferencia.
        candidates = [
            os.path.join(repo_root, 'DriveA5', 'build', 'Release', 'a5_pos_cli.exe'),
            os.path.join(repo_root, 'DriveA5', 'build', 'a5_pos_cli.exe'),
        ]
        # drive_exe recebe o primeiro caminho existente.
        drive_exe = None
        for c in candidates:
            if os.path.exists(c):
                drive_exe = c
                # Para no primeiro valido para evitar ambiguidade de versao.
                break

        if not drive_exe:
            # Sem executavel nao ha como testar serial/Modbus.
            log_msg('Drive check: a5_pos_cli.exe nao encontrado.')
        else:
            # COM5 e padrao do setup atual
            # cmd:
            # - drive_exe: executavel de diagnostico
            # - COM5: porta serial esperada neste setup
            # - --diag: teste basico de comunicacao
            cmd = [drive_exe, 'COM5', '--diag']
            res = subprocess.run(
                cmd,
                # captura stdout/stderr para evitar poluir o console da UI.
                capture_output=True,
                # text=True converte bytes para str automaticamente.
                text=True,
                # timeout curto evita travar o check se a porta nao responder.
                timeout=5,
                # Em Windows, oculta janela de console do subprocess.
                **_subprocess_no_window_kwargs()
            )
            # --diag retorna 0 quando leituras Modbus basicas foram bem-sucedidas.
            # Convencao:
            # - rc=0   -> drive_ok=True
            # - rc!=0  -> drive_ok=False
            drive_ok = (res.returncode == 0)
            if drive_ok:
                log_msg('Drive check: ok.')
            else:
                log_msg('Drive check: falhou (diag).')
    except Exception as e:
        # Erros inesperados sao logados sem interromper o supervisorio.
        log_msg(f'Drive check: erro ({e}).')

    try:
        # Atualiza indicadores visuais de status apos concluir ambos checks.
        _set_status(status_dlg_value, dlg_ok)
        _set_status(status_drive_value, drive_ok)
    except Exception:
        # Ignora erro de UI (ex.: widget indisponivel em transicao de tela).
        pass

# INICIALIZACAO DE ESTADO GLOBAL DO ENSAIO.
# Observacao: este arquivo mistura fluxo atual e da versao anterior; por isso existem
# variaveis historicas usadas em blocos especificos.
#
# aux: tamanho maximo da janela de pontos exibida nos graficos.
aux = 2000
# freq: taxa base para rotinas e escala de tempo visual.
freq = 50.0
# Janela fixa do modo monitoramento para os graficos 1 e 2.
MONITOR_WINDOW_MIN = 1.0
MONITOR_MAX_SAMPLES = 4000
# running: estado geral do ensaio.
running = False
# ip: endereco de referencia para rotinas legadas de hardware.
ip = '127.0.0.1'
# channels: descricao logica de canais para inicializar buffers locais.
# Estrutura de cada item: [ip, tipo_de_canal, reservado].
channels = []
for _ in range(8): channels.append([ip, LDTP_Const.LDTP_CHTYPE_ANIN, 0])
channels.append([ip, LDTP_Const.LDTP_CHTYPE_DIGIN, 0])
numChannels = len(channels)

caminho_arquivo_1 = ""
caminho_arquivo_2 = ""
# Caminhos usados pelo pipeline externo (DLG/Drive/Merge)
caminho_dlg_csv = ""
caminho_drive_csv = ""
caminho_turn_dist_csv = ""
caminho_turn_volta_csv = ""
caminho_merge_csv = ""
caminho_schedule_csv = ""
caminho_graficos_png = ""
graph_events_log_path = ""
graph_events_lock = threading.Lock()
# Arbitro de fonte do grafico 3 em tempo real:
# - prioridade: stream TURN do agregador C
# - fallback: agregacao local (dlg.csv + drive.csv) apenas em stall/falha do stream
turn_rt_lock = threading.Lock()
turn_rt_mode = "stream"
turn_rt_stream_has_data = False
turn_rt_stream_last_ts = 0.0
turn_rt_stream_closed = False
TURN_RT_STREAM_STALL_S = 3.0
TURN_RT_STARTUP_GRACE_S = 4.0

# Inicializa listas de amostras

# widgets da tabela de etapas (entrada do usuario).
lista_entries_velocidade = []
lista_entries_distancia = []
lista_labels_voltas_cursos = []
lista_labels_voltas_pin = []
lista_labels_duracao = []
lista_labels_velocidade_real = []
# allSamps: buffer da versao anterior para escrita local.
allSamps = [[] for _ in range(numChannels)] 
# graSamps: buffer principal consumido por update1/update2/update3.
graSamps = [[] for _ in range(numChannels)]
# sampsTimestamp: eixo temporal associado aos pontos em graSamps.
sampsTimestamp = []
# Estruturas legadas de planejamento de etapas por tensao/tempo.
tensao_vel = []
duracao = []
soma_tempos_vel = []
# Controle do cronometro exibido na UI.
start_time = 0
timer_started = False

# Controle de escala Y (auto por padrao).
# Mantemos valores padrao para evitar NameError.
y1_auto = True
y2_auto = True
y3_auto = True
y1_min = 0.0
y1_max = 1.0
y2_min = 0.0
y2_max = 1.0
y3_min = 0.0
y3_max = 1.0
# Controle de escala X compartilhado entre grafico 1 (Temperatura) e 2 (CoF).
# - x_auto=True  -> eixo X acompanha duracao total prevista do ensaio.
# - x_auto=False -> eixo X usa janela manual em minutos e reinicia por ciclos.
x_auto = True
x_manual_min = 0.83
x_auto_total_min = 0.83
# Controle de escala X do grafico 3 (atrito por distancia).
# - x3_auto=True  -> eixo X acompanha distancia total prevista.
# - x3_auto=False -> eixo X usa janela manual em milimetros e reinicia por ciclos.
x3_auto = True
x3_manual_mm = 100.0
x3_auto_total_mm = 100.0
# Modo de visualizacao do grafico 3: "dist" (distancia) ou "turn" (volta).
graph3_mode = "dist"
# Referencia temporal para converter timestamp absoluto em tempo relativo no grafico.
x_time_ref_s = None
# Forca normal usada para converter CH1 em CoF no grafico 2.
cof_force_normal_n = 1.0

# Dados do grafico 3 (atrito por distancia e por volta).
# - p_strokes/p_atrito_*: serie por distancia (mantida como base historica).
# - p_turn_strokes/p_turn_atrito_*: serie por volta.
p_strokes = []
p_atrito_ef = []
p_atrito_max = []
p_atrito_min = []
p_coluna_velocidade = []

p_turn_strokes = []
p_turn_atrito_ef = []
p_turn_atrito_max = []
p_turn_atrito_min = []
p_turn_coluna_velocidade = []

p_dist_target_mm = 0.0
p_turn_target = 0.0

# Revisoes de renderizacao dos graficos.
# Threads de tail atualizam dados; a UI le essas revisoes no loop Tk e redesenha
# apenas quando ha dado/configuracao nova na aba atualmente visivel.
graph_revision_lock = threading.Lock()
graph_data_revision = 0
graph_config_revision = 0


def _bump_graph_data_revision():
    global graph_data_revision
    try:
        with graph_revision_lock:
            graph_data_revision += 1
    except Exception:
        pass


def _bump_graph_config_revision():
    global graph_config_revision
    try:
        with graph_revision_lock:
            graph_config_revision += 1
    except Exception:
        pass


def _current_graph_revision():
    try:
        with graph_revision_lock:
            return (graph_data_revision, graph_config_revision)
    except Exception:
        return (0, 0)

# Configuracao do tamanho do bloco para salvar no disco (ex: a cada 1000 linhas)
tamanho_bloco = 1000 


# Nicolas
# go():
# - Le amostras de um arquivo *_T.txt (modo de simulacao da versao anterior).
# - Converte cada linha para floats, atualiza buffers de grafico e tempo.
# - Grava em blocos no arquivo de saida para reduzir I/O por linha.
# - Usa variaveis globais:
#   running/is_paused para fluxo, aux/freq para janela temporal,
#   graSamps/sampsTimestamp para plot, caminho_arquivo_2 para persistencia.
def go():
    global running, aux, freq, start_time, contador_amostras_total
    global graSamps, allSamps, sampsTimestamp
    global caminho_arquivo_1, caminho_arquivo_2

    running = "true"
    try:
        label_ensaio_estado.config(text="Em andamento", fg="#0078D4")
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
                    f_in.seek(last_pos) # Volta para o inicio da linha de dados valida
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
                        # Monta uma string para salvar no arquivo de saida txt
                        ## (primeiro valor e o tempo/coluna 0)
                        linha_para_salvar = f"{vals[0]:.3f}" # Timestamp
                        
                        # Loop para percorrer cada canal de sensor (0 a 8)
                        for iCh in range(numChannels):
                            val = vals[iCh + 1] # Pega o valor na lista de valores convertidos
                            
                            # Adiciona ao gráfico
                            if reset_grafico:
                                graSamps[iCh].clear()
                            graSamps[iCh].append(val) # Adiciona o novo valor na lista de plotagem do gráfico
                            
                            linha_para_salvar += f"; {val:.3f}" # Adiciona string do valor

                        _bump_graph_data_revision()

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
            # --- FINALIZACAO ---
            print("Aquisição finalizada.")
            running = False

    except Exception as e:
        print(f"Erro critico na thread go: {e}")
        running = False


# Nicolas
################# funcao grafico para terceiro grafico (ARQUIVO _P)
'''
# Ela abre o arquivo _P e le linha por linha
# O Grafico 3 nao e de tempo continuo, e ponto a ponto (stroke). A funcao le o tempo em que aquele stroke aconteceu e fica em pausa (while) esperando o relogio principal (funcao go) chegar naquele tempo.

#Exemplo: O stroke 5 aconteceu no segundo 10. A função go_p lê isso e espera a simulação chegar no segundo 10. Quando chega, ela libera os dados.
'''

# go_p():
# - Le arquivo *_P.txt com dados por stroke (grafico 3).
# - Sincroniza cada ponto com o tempo produzido por go() usando sampsTimestamp.
# - Atualiza listas p_strokes/p_atrito_* consumidas por update3().
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
                        # EXTRACAO DOS DADOS PARA O GRAFICO
                        p_atrito_ef.append(ef)
                        p_atrito_max.append(mx)
                        p_atrito_min.append(mn)
                        p_coluna_velocidade.append(vel_3graf)
                        
                    except ValueError: pass
                
                
    except Exception as e:
        print(f"Erro na thread go_p: {e}")

##########################################################################################3


# fechar_janela():
# - Envia comando digital da versao anterior para seguranca (SetDigOut).
# - Finaliza o loop da interface Tkinter.
def fechar_janela():
    global graph_render_job
    if monitor_mode_active:
        _stop_monitor_mode()
    try:
        if graph_render_job is not None:
            root.after_cancel(graph_render_job)
            graph_render_job = None
    except Exception:
        pass
    myModule.SetDigOut(ip, 0, 1, 0)
    root.quit()


# salvar_arquivo():
# - Monta caminho de saida em pasta unica por execucao:
#   REPO_BASE/<AAAA-MM-DD - PoD - NomeEnsaio_Estudo-Repeticao>/
# - Bloqueia duplicidade pela combinacao data + nome + estudo + repeticao.
#   Se a pasta existir, solicita confirmacao explicita para sobrescrever.
# - Define nomes de saida para arquivos finais com padrao:
#   <data>-<nome_ensaio>-<estudo>-<repeticao>_<sufixo>.csv/png
#   sufixos: _I (info), _DP (processado por distancia), _VP (processado por volta),
#   _T (resultado ensaio), _G (imagem dos 4 graficos).
# - Durante o ensaio, dlg.csv/drive.csv ficam na pasta da execucao e ao final
#   sao movidos para DadosDev.
# - Inicializa arquivos vazios para evitar erro de append posterior.
def _clear_output_paths(clear_folder=False):
    """
    Limpa caminhos pendentes quando a criacao/sobrescrita do ensaio e cancelada.
    """
    global caminho_arquivo_1, caminho_arquivo_2, caminho_pasta
    global caminho_dlg_csv, caminho_drive_csv, caminho_turn_dist_csv, caminho_turn_volta_csv, caminho_merge_csv, caminho_schedule_csv, caminho_graficos_png
    global graph_events_log_path
    caminho_arquivo_1 = ""
    caminho_arquivo_2 = ""
    caminho_dlg_csv = ""
    caminho_drive_csv = ""
    caminho_turn_dist_csv = ""
    caminho_turn_volta_csv = ""
    caminho_merge_csv = ""
    caminho_schedule_csv = ""
    caminho_graficos_png = ""
    graph_events_log_path = ""
    if clear_folder:
        caminho_pasta = ""


def _remove_run_folder_for_overwrite(folder_path, base_path):
    """
    Remove a pasta de uma repeticao existente com retries e tratamento read-only.

    Se algum arquivo estiver aberto por outro programa, o Windows pode bloquear a
    remocao. Nesse caso, falha com mensagem acionavel em vez de continuar com
    arquivos antigos misturados ao novo ensaio.
    """
    target = os.path.abspath(folder_path)
    base = os.path.abspath(base_path)
    try:
        common = os.path.commonpath([target, base])
    except Exception:
        common = ""
    if target == base or common != base:
        raise RuntimeError("Caminho de sobrescrita invalido.")

    last_err = None

    def _retry_remove(func, path, exc_info):
        nonlocal last_err
        try:
            os.chmod(path, 0o666)
        except Exception:
            pass
        try:
            func(path)
        except Exception as e:
            last_err = e

    for _ in range(8):
        if not os.path.exists(target):
            return
        try:
            try:
                shutil.rmtree(target, onexc=_retry_remove)
            except TypeError:
                shutil.rmtree(target, onerror=_retry_remove)
        except Exception as e:
            last_err = e
        if not os.path.exists(target):
            return
        time.sleep(0.25)

    detail = f" Detalhe: {last_err}" if last_err else ""
    raise RuntimeError(
        "Nao foi possivel remover a pasta existente. "
        "Feche CSVs/imagens abertos no Excel, visualizadores ou Explorer e tente novamente."
        + detail
    )


def salvar_arquivo():
    
    global caminho_arquivo_1, caminho_arquivo_2, caminho_pasta
    global caminho_dlg_csv, caminho_drive_csv, caminho_turn_dist_csv, caminho_turn_volta_csv, caminho_merge_csv, caminho_schedule_csv, caminho_graficos_png
    global graph_events_log_path

    # ---------------------------------------------------------------------------------
    # Estrutura padrao:
    # Pasta base: REPO_BASE
    # Pasta da execucao: "AAAA-MM-DD - PoD - NomeEnsaio_Estudo-Repeticao"
    # Bloqueio de duplicidade considera data + nome + estudo + repeticao.
    # ---------------------------------------------------------------------------------
    nome_ensaio = entries_left["Nome do ensaio"].get().strip()
    estudo = entries_left["Estudo"].get().strip()
    repeticao = entries_left["Repetição"].get().strip()

    # Formato ISO no nome da pasta melhora ordenacao no Explorer.
    data_dir_str = datetime.now().strftime("%Y-%m-%d")
    data_file_str = data_dir_str

    # Normaliza nomes para Windows (sem caracteres proibidos).
    nome_pasta_execucao = orch.sanitize_folder_name(
        f"{data_dir_str} - PoD - {nome_ensaio}_{estudo}-{repeticao}"
    )
    caminho_pasta = os.path.join(REPO_BASE, nome_pasta_execucao)

    # Garante pasta base
    if not os.path.exists(REPO_BASE):
        os.makedirs(REPO_BASE, exist_ok=True)

    # Se a pasta dessa execucao ja existir, pergunta se deve sobrescrever.
    if os.path.exists(caminho_pasta):
        deseja_sobrescrever = messagebox.askyesno(
            "Repeticao ja existe",
            "Ja existe um ensaio com esta combinacao:\n"
            "AAAA-MM-DD + nome + estudo + repeticao.\n\n"
            "Se voce clicar em 'Sim', a pasta atual sera APAGADA e "
            "substituida pelos novos dados.\n\n"
            "Deseja sobrescrever esta repeticao?",
            icon="warning",
            default="no",
        )
        if not deseja_sobrescrever:
            _clear_output_paths(clear_folder=True)
            return

        confirma_apagar = messagebox.askyesno(
            "Confirmacao final",
            "Confirmar sobrescrita da repeticao?\n\n"
            "Todos os arquivos existentes nessa pasta serao removidos.",
            icon="warning",
            default="no",
        )
        if not confirma_apagar:
            _clear_output_paths(clear_folder=True)
            return

        try:
            _remove_run_folder_for_overwrite(caminho_pasta, REPO_BASE)
        except Exception as e:
            messagebox.showerror(
                "Erro ao sobrescrever",
                f"Nao foi possivel apagar a repeticao existente.\n\n{e}"
            )
            _clear_output_paths(clear_folder=True)
            return

    os.makedirs(caminho_pasta, exist_ok=True)

    # Prefixo dos arquivos finais solicitado:
    # data + "-" + nome_ensaio + "-" + estudo + "-" + repeticao.
    prefixo_arquivo = orch.sanitize_folder_name(
        f"{data_file_str}-{nome_ensaio}-{estudo}-{repeticao}"
    )

    # Arquivos finais visiveis ao usuario.
    caminho_arquivo_1 = os.path.join(caminho_pasta, f"{prefixo_arquivo}_I.csv")   # metadados do ensaio
    caminho_turn_dist_csv = os.path.join(caminho_pasta, f"{prefixo_arquivo}_DP.csv")
    caminho_turn_volta_csv = os.path.join(caminho_pasta, f"{prefixo_arquivo}_VP.csv")
    caminho_merge_csv = os.path.join(caminho_pasta, f"{prefixo_arquivo}_T.csv")
    caminho_graficos_png = os.path.join(caminho_pasta, f"{prefixo_arquivo}_G.png")

    # Arquivos tecnicos de aquisicao (movidos para DadosDev no fim).
    caminho_arquivo_2 = os.path.join(caminho_pasta, "dlg.csv")    # mantido para compatibilidade
    caminho_dlg_csv = os.path.join(caminho_pasta, "dlg.csv")
    caminho_drive_csv = os.path.join(caminho_pasta, "drive.csv")
    caminho_schedule_csv = os.path.join(caminho_pasta, "schedule.csv")
    graph_events_log_path = os.path.join(caminho_pasta, "graph_events.log")

    # Cria arquivos vazios (evita erros de permissao na hora do append)
    for p in [caminho_arquivo_1, caminho_arquivo_2, graph_events_log_path]:
        with open(p, "w", encoding="utf-8") as f:
            f.write("")
   


# converte():
# - Stub da versao anterior (desativado).
# - Historicamente chamaria conversor.exe para etapa de pos-processamento.
def converte(): 
    pass

# gera_grafico():
# - Stub da versao anterior (desativado).
# - Historicamente chamaria grafico3_9.exe apos o ensaio.
def gera_grafico():
    pass
# velocidades():
# - Executa cronograma de velocidades no modo da versao anterior (analogico/LDTP).
# - Entrada: lista_velocidades_digitadas e lista_duracao (segundos).
# - Para cada etapa: calcula rpm de referencia para UI, envia SetAnOutV
#   e respeita pausa/parada zerando tensao quando necessario.
# - Efeito colateral principal: comando direto de hardware via myModule.
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
            
        # 1. Pega os dados dessa etapa especifica
        vel_atual_mms = lista_velocidades_digitadas[i]  # Valor para mostrar (mm/s)
        duracao_atual_s = lista_duracao[i]              # Duração dessa etapa (segundos)

        # 2. Atualiza os labels de alvo atual (velocidade linear e RPM alvo)
        try:
            raio_mm = float(ent_raio.get().strip().replace(",", "."))
        except Exception:
            raio_mm = 0.0
        # rpm_from_mm_s(v, raio, relacao) retorna int rpm para exibir/enviar.
        rpm_alvo = orch.rpm_from_mm_s(vel_atual_mms, raio_mm, RELACAO_MECANICA) if raio_mm > 0 else 0
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

            # SE ESTIVER RODANDO (NAO PAUSADO):
            else:
                # Se estava parado antes, religa o motor na velocidade certa
                if motor_estava_parado:
                    myModule.SetAnOutV(ip, tensao_para_enviar, 0, 100)
                    _set_target_labels(f"{vel_atual_mms} mm/s", f"{rpm_alvo} rpm")
                    motor_estava_parado = False
                passo = 0.1
                if passo > tempo_rest: 
                    passo = tempo_rest
                
                time.sleep(passo)
                tempo_rest -= passo

        if running != "true":
            break

    # Quando terminar tudo zera
    if running == "true":
        try:
            label_ensaio_estado.config(text="Finalizado", fg="green")
            _set_targets_stopped()
        except Exception:
            pass
        myModule.SetAnOutV(ip, 0, 0, 100) # Para a máquina

        running = False 
        is_paused = False
        
        # Reseta o visual do botão Pausar
        if 'button_frame5_pausar' in globals():
            button_frame5_pausar.config(text="Pausar", bg="SystemButtonFace")



# _format_hms(seconds):
# - Recebe segundos (int/float/texto conversivel) e devolve HH:MM:SS.
# - Garante limites minimos (nunca retorna tempo negativo).
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

# atualiza_decorrido():
# - Atualiza label de tempo da UI a cada ~200 ms.
# - Considera pausa (congela contador) e opcionalmente calcula restante
#   quando existe duracao total do schedule.
# - Usa start_time/timer_started/is_paused/tempo_pause_inicio.
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
        if not timer_started:
            elapsed = 0
        elif is_paused:
            elapsed = tempo_pause_inicio - start_time
        else:
            elapsed = time.time() - start_time
    except Exception:
        elapsed = 0
    if elapsed < 0:
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

# _start_timer_now():
# - Marca start_time com time.time() e habilita timer_started.
# - Ponto unico para "iniciar cronometro oficial" do ensaio.
def _start_timer_now():
    """Inicia o cronometro no instante atual (thread-safe via root.after)."""
    global start_time, timer_started
    start_time = time.time()
    timer_started = True
    atualiza_decorrido()

# _is_running():
# - Abstracao para tratar comportamento da versao anterior: running pode ser bool ou string "true".
# - Retorna bool padrao para facilitar condicionais.
def _is_running():
    return (running == "true") or (running is True)


def _run_token_alive(run_token):
    """
    Valida se a thread pertence ao ensaio externo atual.

    Isso evita que tail threads antigas sigam alimentando graficos quando
    um novo ensaio e iniciado.
    """
    if run_token is None:
        return True
    return run_token == external_run_token


def _read_complete_tail_line(f):
    """
    Le uma linha completa de um arquivo em crescimento.

    Retorno:
      (line, True)  -> linha completa (terminada em '\\n')
      ("", False)   -> sem dado novo ou linha parcial (writer ainda escrevendo)
    """
    try:
        pos = f.tell()
        line = f.readline()
    except Exception:
        return "", False

    if not line:
        return "", False

    # Em tail no Windows, pode surgir linha parcial no EOF enquanto writer escreve.
    if not line.endswith("\n"):
        try:
            f.seek(pos)
        except Exception:
            pass
        return "", False

    return line, True

# _set_target_labels(vel_txt, rpm_txt):
# - Atualiza os textos de "Velocidade alvo atual" e "RPM Alvo atual".
# - Faz update thread-safe via root.after quando possivel.
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

# _set_targets_stopped():
# - Conveniencia para retornar labels de alvo ao estado parado.
def _set_targets_stopped():
    _set_target_labels("0 mm/s", "0 rpm")


def _abort_external_run_due_dlg_timeout(reason_msg):
    """
    Aborta o ensaio externo quando o DLG nao entrega amostras validas no inicio.

    Motivo:
    - Evita executar todo o ensaio com dlg.csv quase todo em erro/NULL.
    - Mantem comportamento previsivel: sem DLG valido, ensaio nao prossegue.
    """
    global running, is_paused, tempo_pause_inicio, timer_started, external_run_state
    if not _is_running() or timer_started:
        return

    log_msg(reason_msg)
    running = False
    is_paused = False
    tempo_pause_inicio = 0
    timer_started = False

    state_snapshot = external_run_state
    external_run_state = None
    if state_snapshot is not None:
        def _stop_external():
            try:
                orch.stop_run(state_snapshot)
            except Exception as e:
                log_msg(f"Falha ao encerrar subprocessos apos timeout do DLG: {e}")
        threading.Thread(target=_stop_external, daemon=True).start()

    try:
        label_ensaio_estado.config(text="Falha DLG", fg="red")
        _set_targets_stopped()
        lbl_tempo_decorrido2.config(text="0:00:00")
        if 'button_frame5_pausar' in globals():
            button_frame5_pausar.config(text="Pausar", bg="SystemButtonFace")
    except Exception:
        pass

    try:
        messagebox.showerror(
            "Falha de aquisicao DLG",
            "O DLG nao entregou amostras validas para iniciar o ensaio.\n"
            "Verifique comunicacao/rede e execute novamente."
        )
    except Exception:
        pass

# _update_vel_label_from_schedule(...):
# - Thread de UI que acompanha o tempo decorrido e seleciona etapa ativa.
# - Mostra mm/s e rpm alvo correspondentes ao schedule atual.
# - Le arrays de velocidade/duracao e opcionalmente lista de rpm pronta.
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
        if is_paused:
            time.sleep(0.1)
            continue
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
# _wait_dlg_ok_and_start_timer(...):
# - Monitora dlg.csv ate observar N linhas validas (err=0).
# - So entao inicia o cronometro oficial, evitando "tempo correndo sem dado".
# - Parametros:
#   dlg_csv_path: arquivo monitorado.
#   min_ok: numero minimo de amostras validas.
#   timeout_s: limite maximo de espera.
def _wait_dlg_ok_and_start_timer(dlg_csv_path, min_ok=3, timeout_s=15, run_token=None):
    """
    Aguarda N amostras validas do DLG (err=0) e so entao inicia o cronometro.
    Isso alinha o tempo do GUI com o inicio real do DLG.
    """
    log_msg(f"Aguardando {min_ok} amostras validas do DLG para iniciar tempo...")
    t0 = time.time()
    ok_count = 0
    seen_rows = 0

    # Espera o arquivo existir
    while _is_running() and _run_token_alive(run_token) and not os.path.exists(dlg_csv_path):
        if time.time() - t0 > timeout_s:
            try:
                root.after(0, _abort_external_run_due_dlg_timeout,
                           "DLG: timeout aguardando arquivo CSV de aquisicao.")
            except Exception:
                _abort_external_run_due_dlg_timeout("DLG: timeout aguardando arquivo CSV de aquisicao.")
            return
        time.sleep(0.05)

    try:
        with open(dlg_csv_path, "r", encoding="utf-8") as f:
            # Pula cabecalho se existir
            first = f.readline()
            if first and "idx" not in first:
                # Linha nao era cabecalho; processa como dado
                f.seek(0)

            while _is_running() and _run_token_alive(run_token):
                line, complete = _read_complete_tail_line(f)
                if not complete:
                    if time.time() - t0 > timeout_s:
                        try:
                            root.after(0, _abort_external_run_due_dlg_timeout,
                                       "DLG: timeout aguardando amostras validas (err=0).")
                        except Exception:
                            _abort_external_run_due_dlg_timeout("DLG: timeout aguardando amostras validas (err=0).")
                        return
                    # Keep tailing while producer appends (Windows EOF refresh).
                    try:
                        f.seek(f.tell())
                    except Exception:
                        pass
                    time.sleep(0.05)
                    continue

                parts = line.strip().split(",")
                if not parts:
                    continue
                seen_rows += 1

                # DLG CSV: ... , err (ultima coluna)
                if parts[-1].strip() == "0":
                    ok_count += 1
                    if ok_count == 1:
                        graph_log(f"TIMER gate first valid row idx={parts[0]} total_seen={seen_rows}")
                    if ok_count >= min_ok:
                        while _is_running() and _run_token_alive(run_token) and is_paused:
                            time.sleep(0.05)
                        if not _is_running() or timer_started:
                            return
                        try:
                            root.after(0, _start_timer_now)
                        except Exception:
                            _start_timer_now()
                        log_msg("DLG: amostras validas confirmadas; tempo iniciado.")
                        return
    except Exception as e:
        try:
            root.after(0, _abort_external_run_due_dlg_timeout,
                       f"DLG: erro aguardando amostras validas ({e}).")
        except Exception:
            _abort_external_run_due_dlg_timeout(f"DLG: erro aguardando amostras validas ({e}).")

# _tail_dlg_csv_for_graphs(...):
# - Faz tail do dlg.csv em tempo real durante ensaio externo.
# - Converte linhas para buffers de plot (graSamps/sampsTimestamp).
# - Em err=1 ou NULL, grava NaN para manter alinhamento temporal.
def _tail_dlg_csv_for_graphs(dlg_csv_path, run_token=None):
    """
    Alimenta os graficos com dados do DLG (modo externo).
    Lê dlg.csv em tempo real e atualiza graSamps + sampsTimestamp.
    """
    # Espera o arquivo existir
    t0 = time.time()
    while _is_running() and _run_token_alive(run_token) and not os.path.exists(dlg_csv_path):
        if time.time() - t0 > 15:
            log_msg("DLG: timeout aguardando dlg.csv para graficos.")
            return
        time.sleep(0.05)

    try:
        log_msg(f"DLG tail: start token={run_token}.")
        n_rows = 0
        n_short = 0
        n_err = 0
        n_extreme_ch1 = 0
        n_extreme_ch2 = 0
        first_valid_logged = False
        with open(dlg_csv_path, "r", encoding="utf-8") as f:
            # Pula cabecalho
            header = f.readline()
            if header and "idx" not in header:
                f.seek(0)

            while _is_running() and _run_token_alive(run_token):
                line, complete = _read_complete_tail_line(f)
                if not complete:
                    # Keep tailing while producer appends (Windows EOF refresh).
                    try:
                        f.seek(f.tell())
                    except Exception:
                        pass
                    time.sleep(0.02)
                    continue

                parts = line.strip().split(",")
                # Esperado: idx,t_qpc,t_s,ch1..ch8,atrito,err (atrito opcional em versoes antigas)
                if len(parts) < 12:
                    n_short += 1
                    continue

                try:
                    t_s = float(parts[2])
                except Exception:
                    continue

                err = parts[-1].strip()
                if err != "0":
                    n_err += 1
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
                _bump_graph_data_revision()
                n_rows += 1
                if err == "0" and not first_valid_logged:
                    graph_log(f"DLG first valid row: idx={parts[0]} t_s={t_s:.6f} ch1={vals[0]} ch2={vals[1]}")
                    first_valid_logged = True
                try:
                    if err == "0" and not math.isnan(vals[0]) and abs(vals[0]) > 2000.0:
                        n_extreme_ch1 += 1
                        if n_extreme_ch1 <= 10:
                            graph_log(f"DLG extreme CH1 idx={parts[0]} t_s={t_s:.6f} v={vals[0]:.6f}")
                    if err == "0" and not math.isnan(vals[1]) and abs(vals[1]) > 2000.0:
                        n_extreme_ch2 += 1
                        if n_extreme_ch2 <= 10:
                            graph_log(f"DLG extreme CH2 idx={parts[0]} t_s={t_s:.6f} v={vals[1]:.6f}")
                except Exception:
                    pass
                if n_rows % 1000 == 0:
                    log_msg(f"DLG tail: token={run_token} rows={n_rows}.")
                if n_rows % 500 == 0:
                    graph_log(
                        "DLG tail stats rows={0} err={1} short={2} ext_ch1={3} ext_ch2={4}".format(
                            n_rows, n_err, n_short, n_extreme_ch1, n_extreme_ch2
                        )
                    )

                # Mantem historico completo do ensaio para permitir:
                # - alternar manual/automatico no eixo X sem perder o passado;
                # - voltar ao modo automatico e replotar toda a serie.
        log_msg(f"DLG tail: stop token={run_token} rows={n_rows}.")
        graph_log(
            "DLG tail stop rows={0} err={1} short={2} ext_ch1={3} ext_ch2={4}".format(
                n_rows, n_err, n_short, n_extreme_ch1, n_extreme_ch2
            )
        )
    except Exception as e:
        log_msg(f"DLG: erro lendo dlg.csv para graficos ({e}).")


def _tail_monitor_csv_for_graphs(dlg_csv_path, monitor_token):
    """
    Faz tail do dlg.csv no modo monitoramento (sem ensaio).

    Objetivo:
    - Exibir CH1/CH2 em tempo real com janela fixa de 1 minuto.
    - Nao gerar efeitos no pipeline de ensaio.
    """
    t0 = time.time()
    while monitor_mode_active and (monitor_token == monitor_mode_token) and not os.path.exists(dlg_csv_path):
        if time.time() - t0 > 10.0:
            log_msg("Monitoramento: timeout aguardando csv do DLG.")
            return
        time.sleep(0.05)

    try:
        with open(dlg_csv_path, "r", encoding="utf-8") as f:
            header = f.readline()
            if header and "idx" not in header.lower():
                f.seek(0)

            while monitor_mode_active and (monitor_token == monitor_mode_token):
                line, complete = _read_complete_tail_line(f)
                if not complete:
                    try:
                        f.seek(f.tell())
                    except Exception:
                        pass
                    if monitor_proc is not None and monitor_proc.poll() is not None:
                        break
                    time.sleep(0.02)
                    continue

                parts = line.strip().split(",")
                if len(parts) < 12:
                    continue
                try:
                    t_s = float(parts[2])
                except Exception:
                    continue

                err = parts[-1].strip()
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

                # Mantem apenas historico suficiente para 1 min + margem.
                while len(sampsTimestamp) > MONITOR_MAX_SAMPLES:
                    sampsTimestamp.pop(0)
                    for i in range(8):
                        if graSamps[i]:
                            graSamps[i].pop(0)
                _bump_graph_data_revision()
    except Exception as e:
        log_msg(f"Monitoramento: erro no tail do DLG ({e}).")


def _start_monitor_mode():
    """
    Inicia modo monitoramento de CH1/CH2 sem ensaio ativo.
    """
    global monitor_mode_active, monitor_mode_token, monitor_proc, monitor_tail_thread, monitor_csv_path
    global x_time_ref_s

    if monitor_mode_active:
        return True

    if _is_running() or _external_pipeline_ativo() or tara_running:
        messagebox.showwarning(
            "Monitoramento",
            "Nao e possivel ativar monitoramento durante ensaio/tara."
        )
        return False

    repo_root = orch.find_repo_root()
    exe_info = orch.check_executables(repo_root)
    dlg_exe = exe_info.get("dlg_exe")
    if not dlg_exe:
        messagebox.showerror(
            "Monitoramento",
            "dlg_logger_ipc.exe nao encontrado.\n\nCompile os executaveis C antes de usar monitoramento."
        )
        return False

    mon_dir = os.path.join(tempfile.gettempdir(), "LATRIB_monitor")
    os.makedirs(mon_dir, exist_ok=True)
    monitor_csv_path = os.path.join(
        mon_dir, f"dlg_monitor_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
    )

    # Limpa buffers para comecar o monitor com serie limpa.
    sampsTimestamp.clear()
    for i in range(numChannels):
        graSamps[i].clear()
    x_time_ref_s = None
    _bump_graph_config_revision()

    cmd = [
        dlg_exe,
        "--out", monitor_csv_path,
        "--duration", "86400",
        "--rate", f"{float(getattr(orch, 'DEFAULT_RATE_HZ', 50.0)):.0f}",
        "--ip", str(getattr(orch, "DEFAULT_DLG_IP", "192.168.1.100")),
        "--port", str(getattr(orch, "DEFAULT_DLG_PORT", 41401)),
        "--bind-port", "0",
        "--ipc",
    ]

    try:
        monitor_proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            **_subprocess_no_window_kwargs()
        )

        ready = True
        if hasattr(orch, "_wait_ready"):
            ready = orch._wait_ready(monitor_proc, "DLG monitor", timeout_s=8.0)
        if not ready:
            raise RuntimeError("DLG monitor nao respondeu READY.")

        if hasattr(orch, "_send_start"):
            orch._send_start(monitor_proc)
        else:
            monitor_proc.stdin.write("START\n")
            monitor_proc.stdin.flush()
    except Exception as e:
        try:
            if monitor_proc and monitor_proc.poll() is None:
                monitor_proc.terminate()
        except Exception:
            pass
        monitor_proc = None
        log_msg(f"Monitoramento: falha ao iniciar ({e}).")
        messagebox.showerror("Monitoramento", f"Falha ao iniciar monitoramento.\n\n{e}")
        return False

    monitor_mode_active = True
    monitor_mode_token += 1
    tk = monitor_mode_token
    monitor_tail_thread = threading.Thread(
        target=_tail_monitor_csv_for_graphs,
        args=(monitor_csv_path, tk),
        daemon=True
    )
    monitor_tail_thread.start()
    _set_start_button_state()
    try:
        label_ensaio_estado.config(text="Monitoramento", fg="#0078D4")
    except Exception:
        pass
    log_msg("Monitoramento: ativo (CH1/CH2, janela fixa 1 min).")
    return True


def _stop_monitor_mode():
    """
    Encerra monitoramento e libera inicio de ensaio.
    """
    global monitor_mode_active, monitor_proc, monitor_tail_thread, monitor_csv_path
    global x_time_ref_s

    monitor_mode_active = False

    try:
        if monitor_proc is not None and monitor_proc.poll() is None:
            if hasattr(orch, "_send_ipc"):
                orch._send_ipc(monitor_proc, "STOP")
            else:
                monitor_proc.stdin.write("STOP\n")
                monitor_proc.stdin.flush()
            try:
                monitor_proc.wait(timeout=2.0)
            except Exception:
                try:
                    monitor_proc.terminate()
                    monitor_proc.wait(timeout=1.0)
                except Exception:
                    try:
                        monitor_proc.kill()
                    except Exception:
                        pass
    except Exception:
        pass

    monitor_proc = None
    monitor_tail_thread = None
    if "monitor_mode_var" in globals():
        try:
            monitor_mode_var.set(False)
        except Exception:
            pass
    _set_start_button_state()
    if not _is_running():
        try:
            label_ensaio_estado.config(text="Aguardando novo ensaio", fg="black")
        except Exception:
            pass

    # Limpa buffers de monitor para evitar confusao visual ao sair do modo.
    sampsTimestamp.clear()
    for i in range(numChannels):
        graSamps[i].clear()
    x_time_ref_s = None
    _bump_graph_config_revision()

    if monitor_csv_path:
        try:
            os.remove(monitor_csv_path)
        except Exception:
            pass
        monitor_csv_path = ""

    log_msg("Monitoramento: desativado.")


def toggle_monitor_mode():
    """
    Callback do bit de monitoramento na aba principal.
    """
    if "monitor_mode_var" not in globals():
        return

    if bool(monitor_mode_var.get()):
        if _is_running() or _external_pipeline_ativo() or tara_running:
            messagebox.showwarning(
                "Monitoramento",
                "Nao e possivel ativar monitoramento durante ensaio/tara."
            )
            monitor_mode_var.set(False)
            return
        ok = _start_monitor_mode()
        if not ok:
            monitor_mode_var.set(False)
    else:
        _stop_monitor_mode()


def _calc_expected_distance_mm_from_table():
    """
    Soma a distancia total prevista da tabela inferior (mm).

    Regras:
    - Soma apenas linhas com velocidade e distancia validas.
    - Ignora linhas vazias e valores <= 0.
    """
    total_dist_mm = 0.0
    for i in range(11):
        vel_txt = lista_entries_velocidade[i].get().strip().replace(",", ".")
        dist_txt = lista_entries_distancia[i].get().strip().replace(",", ".")
        if not vel_txt or not dist_txt:
            continue
        try:
            vel = float(vel_txt)
            dist = float(dist_txt)
        except Exception:
            continue
        if vel <= 0 or dist <= 0:
            continue
        total_dist_mm += dist
    return total_dist_mm


def _calc_expected_pin_turns_from_table(raio_mm):
    """
    Estima total de voltas fisicas do pino a partir da distancia total prevista.
    """
    if raio_mm <= 0:
        return 0.0
    total_dist_mm = _calc_expected_distance_mm_from_table()
    if total_dist_mm <= 0:
        return 0.0
    return total_dist_mm / (2.0 * math.pi * raio_mm)


def _num_or_nan(txt):
    txt = txt.strip()
    if not txt or txt.upper() == "NULL":
        return float("nan")
    try:
        return float(txt)
    except Exception:
        return float("nan")


def _rpm_motor_to_pin_speed_mm_s(rpm_arr, raio_mm, relacao_mecanica):
    """
    Converte RPM do motor para velocidade linear no pino (mm/s).

    Modelo fisico usado:
    - i = D2 / D1 (D1 motor, D2 disco).
    - rpm_disco = rpm_motor / i
    - v_pino = |rpm_disco| * (2*pi*raio) / 60

    Args:
        rpm_arr (array-like): serie de RPM medio por intervalo (lado motor).
        raio_mm (float): raio da trilha do pino em mm.
        relacao_mecanica (float): i = D2 / D1.

    Returns:
        np.ndarray: serie de velocidade em mm/s (NaN para entradas invalidas).
    """
    try:
        arr = np.asarray(rpm_arr, dtype=float)
    except Exception:
        return np.array([])
    if arr.size == 0:
        return np.array([])
    vel0 = orch.pin_speed_mm_s_from_rpm(0.0, raio_mm, relacao_mecanica)
    if vel0 is None:
        return np.full(arr.shape, np.nan, dtype=float)
    return np.abs(arr) * (2.0 * math.pi * float(raio_mm)) / (60.0 * float(relacao_mecanica))


def _preview_real_pin_speed_mm_s(vel_mm_s, raio_mm):
    """
    Retorna (rpm_inteiro, velocidade_real_mm_s) para a tabela previa.
    """
    if raio_mm is None or raio_mm <= 0 or RELACAO_MECANICA <= 0:
        return None, None
    rpm = orch.rpm_from_mm_s(vel_mm_s, raio_mm, RELACAO_MECANICA)
    vel_real = orch.pin_speed_mm_s_from_rpm(rpm, raio_mm, RELACAO_MECANICA)
    return rpm, vel_real


def _append_dist_point(dist_mm, atr_med, atr_min, atr_max, rpm_med):
    """
    Adiciona ponto do grafico 3 com deduplicacao por distancia crescente.
    """
    if p_strokes:
        try:
            if float(dist_mm) <= float(p_strokes[-1]):
                graph_log(f"DIST drop non-monotonic: incoming={dist_mm} last={p_strokes[-1]}")
                return False
            gap = float(dist_mm) - float(p_strokes[-1])
            gap_warn = max(1.2, float(DIST_INTERVAL_MM) * 1.5)
            if gap > gap_warn:
                graph_log(f"DIST gap detected: last={p_strokes[-1]} incoming={dist_mm} gap_mm={gap:.3f}")
        except Exception:
            return False
    if not math.isnan(atr_max) and abs(atr_max) > 2000.0:
        graph_log(f"DIST extreme atr_max={atr_max:.6f} dist_mm={dist_mm}")
    p_strokes.append(dist_mm)
    p_atrito_ef.append(atr_med)
    p_atrito_min.append(atr_min)
    p_atrito_max.append(atr_max)
    p_coluna_velocidade.append(rpm_med)
    _bump_graph_data_revision()
    return True


def _append_turn_point(turn_n, atr_med, atr_min, atr_max, rpm_med):
    """
    Adiciona ponto do grafico 3 (modo por volta) com deduplicacao crescente.
    """
    if p_turn_strokes:
        try:
            if float(turn_n) <= float(p_turn_strokes[-1]):
                graph_log(f"TURN drop non-monotonic: incoming={turn_n} last={p_turn_strokes[-1]}")
                return False
            gap = float(turn_n) - float(p_turn_strokes[-1])
            if gap > 1.2:
                graph_log(f"TURN gap detected: last={p_turn_strokes[-1]} incoming={turn_n} gap={gap:.3f}")
        except Exception:
            return False
    if not math.isnan(atr_max) and abs(atr_max) > 2000.0:
        graph_log(f"TURN extreme atr_max={atr_max:.6f} turn={turn_n}")
    p_turn_strokes.append(turn_n)
    p_turn_atrito_ef.append(atr_med)
    p_turn_atrito_min.append(atr_min)
    p_turn_atrito_max.append(atr_max)
    p_turn_coluna_velocidade.append(rpm_med)
    _bump_graph_data_revision()
    return True


def _tail_turn_stdout_for_graph3(turn_proc, run_token=None):
    """
    Consome eventos de agregacao emitidos por processo externo e atualiza o grafico 3.
    Formato esperado no stdout:
      DIST,dist_mm,atrito_med,atrito_min,atrito_max,rpm_medio_intervalo,n_total,n_falhas,n_validas,pct_perda
    """
    if turn_proc is None or turn_proc.stdout is None:
        return

    first_logged = False
    try:
        log_msg(f"Turn stream: start token={run_token}.")
        n_turn = 0
        n_turn_ok = 0
        n_turn_drop = 0
        n_turn_extreme = 0
        n_turn_ignored = 0
        while _is_running() and _run_token_alive(run_token):
            line = turn_proc.stdout.readline()
            if not line:
                if turn_proc.poll() is not None:
                    break
                time.sleep(0.05)
                continue
            line = line.strip()
            if not line:
                continue
            if not (line.startswith("DIST,") or line.startswith("TURN,")):
                if line != "READY":
                    log_msg(f"Turnos: {line}")
                continue
            is_turn = line.startswith("TURN,")
            is_dist = line.startswith("DIST,")

            parts = line.split(",")
            if len(parts) < 10:
                continue
            try:
                x_val = float(parts[1])
            except Exception:
                continue

            _turn_rt_mark_stream_data()

            atr_med = _num_or_nan(parts[2])
            atr_min = _num_or_nan(parts[3])
            atr_max = _num_or_nan(parts[4])
            rpm_med = _num_or_nan(parts[5])

            if _turn_rt_get_mode() != "stream":
                n_turn_ignored += 1
            else:
                appended = False
                if is_turn:
                    appended = _append_turn_point(x_val, atr_med, atr_min, atr_max, rpm_med)
                elif is_dist:
                    appended = _append_dist_point(x_val, atr_med, atr_min, atr_max, rpm_med)
                if appended:
                    n_turn_ok += 1
                    if not first_logged:
                        log_msg("Grafico 3: primeiro ponto recebido no stream.")
                        first_logged = True
                    if not math.isnan(atr_max) and abs(atr_max) > 2000.0:
                        n_turn_extreme += 1
                else:
                    n_turn_drop += 1
            n_turn += 1
            if n_turn % 20 == 0:
                graph_log(
                    "GRAPH3 stream stats read={0} ok={1} drop={2} ignored={3} extreme={4} last_x={5} mode={6}".format(
                        n_turn, n_turn_ok, n_turn_drop, n_turn_ignored, n_turn_extreme, x_val,
                        ("TURN" if is_turn else "DIST")
                    )
                )
        log_msg(f"Turn stream: stop token={run_token} turns={n_turn}.")
        _turn_rt_mark_stream_closed()
        graph_log(
            "GRAPH3 stream stop read={0} ok={1} drop={2} ignored={3} extreme={4}".format(
                n_turn, n_turn_ok, n_turn_drop, n_turn_ignored, n_turn_extreme
            )
        )
    except Exception as e:
        log_msg(f"Turnos: erro lendo stream do agregador ({e}).")
        _turn_rt_mark_stream_closed()


def _tail_turn_csv_for_graph3(turn_csv_path, turn_proc=None):
    """
    Faz tail de arquivo _P e alimenta o grafico 3 em tempo real.

    Formatos aceitos:
      distancia_mm;t_s_inicio;atrito_med;atrito_min;atrito_max;rpm_medio_intervalo;...
      volta_n;t_s_inicio;atrito_med;atrito_min;atrito_max;rpm_medio_volta;...
      distancia_mm;atrito_med;atrito_min;atrito_max;rpm_medio_intervalo;... (legado)
      volta_n;atrito_med;atrito_min;atrito_max;rpm_medio_volta;... (legado)
    """
    waited_s = 0.0
    while _is_running() and not os.path.exists(turn_csv_path):
        time.sleep(0.05)
        waited_s += 0.05
        if waited_s >= 15.0:
            log_msg("Distancia: aguardando arquivo _P...")
            waited_s = 0.0

    try:
        with open(turn_csv_path, "r", encoding="utf-8") as f:
            header = f.readline()
            header_l = header.lower() if header else ""
            # Aceita cabecalhos antigos/novos sem depender de caixa.
            is_turn_file = ("volta_n" in header_l)
            is_dist_file = ("distancia_mm" in header_l) or (not is_turn_file)
            has_t_start_col = ("t_s_inicio" in header_l)
            if header and (not is_turn_file and not is_dist_file):
                f.seek(0)
                is_dist_file = True
                has_t_start_col = False

            first_logged = False
            n_rows = 0
            n_ok = 0
            n_drop = 0

            while _is_running():
                line, complete = _read_complete_tail_line(f)
                if not complete:
                    if turn_proc is not None and turn_proc.poll() is not None:
                        # Process ended: try one last refresh to drain trailing lines.
                        try:
                            f.seek(f.tell())
                            line = f.readline()
                        except Exception:
                            line = ""
                        if not line:
                            break
                    # On Windows, tailing a file that is still being appended may
                    # require a no-op seek after EOF so new data becomes visible.
                    try:
                        f.seek(f.tell())
                    except Exception:
                        pass
                    time.sleep(0.05)
                    continue
                parts = line.strip().split(";")
                value_offset = 1 if has_t_start_col else 0
                if len(parts) < (5 + value_offset):
                    continue
                try:
                    x_val = float(parts[0])
                except Exception:
                    continue

                atr_med = _num_or_nan(parts[1 + value_offset])
                atr_min = _num_or_nan(parts[2 + value_offset])
                atr_max = _num_or_nan(parts[3 + value_offset])
                rpm_med = _num_or_nan(parts[4 + value_offset])

                n_rows += 1
                appended = _append_turn_point(x_val, atr_med, atr_min, atr_max, rpm_med) if is_turn_file else _append_dist_point(x_val, atr_med, atr_min, atr_max, rpm_med)
                if appended:
                    n_ok += 1
                    if not first_logged:
                        log_msg("Grafico 3: primeiro ponto recebido via tail CSV.")
                        first_logged = True
                else:
                    n_drop += 1
                if n_rows % 20 == 0:
                    graph_log(
                        f"GRAPH3 csv stats rows={n_rows} ok={n_ok} drop={n_drop} last_x={x_val} mode={'TURN' if is_turn_file else 'DIST'}"
                    )
    except Exception as e:
        log_msg(f"Distancia: erro lendo arquivo _P ({e}).")


def _tail_realtime_dist_from_logs(
    dlg_csv_path,
    drive_csv_path,
    relacao_mecanica,
    raio_mm,
    dist_interval_mm,
    cycles_per_motor_rev=1.0,
    run_token=None
):
    """
    Calcula atrito por distancia e por volta em tempo real de dlg.csv + drive.csv.

    Definicao fisica usada:
    - i = D2 / D1 (D1=motor, D2=disco).
    - voltas_pino = voltas_motor / i.
    - distancia_incremental = voltas_pino * (2*pi*raio).
    """
    if relacao_mecanica <= 0:
        relacao_mecanica = 1.0
    if cycles_per_motor_rev <= 0:
        cycles_per_motor_rev = 1.0
    if raio_mm <= 0:
        log_msg("Distancia RT: raio invalido para agregacao do grafico 3.")
        return
    if dist_interval_mm <= 0:
        dist_interval_mm = 1.0
    rpm_dir_deadband = 5.0

    def _acc_reset():
        return {
            "n_total": 0,
            "n_valid": 0,
            "n_fail": 0,
            "rpm_cnt": 0,
            "atr_sum": 0.0,
            "atr_min": None,
            "atr_max": None,
            "rpm_sum": 0.0,
        }

    def _emit_dist_point(dist_mm, acc_data):
        atr_med = float("nan")
        atr_min = float("nan")
        atr_max = float("nan")
        rpm_med = float("nan")
        if acc_data["n_valid"] > 0:
            atr_med = acc_data["atr_sum"] / float(acc_data["n_valid"])
            atr_min = acc_data["atr_min"] if acc_data["atr_min"] is not None else float("nan")
            atr_max = acc_data["atr_max"] if acc_data["atr_max"] is not None else float("nan")
        if acc_data["rpm_cnt"] > 0:
            rpm_med = acc_data["rpm_sum"] / float(acc_data["rpm_cnt"])
        return _append_dist_point(dist_mm, atr_med, atr_min, atr_max, rpm_med)

    def _emit_turn_point(turn_n, acc_data):
        atr_med = float("nan")
        atr_min = float("nan")
        atr_max = float("nan")
        rpm_med = float("nan")
        if acc_data["n_valid"] > 0:
            atr_med = acc_data["atr_sum"] / float(acc_data["n_valid"])
            atr_min = acc_data["atr_min"] if acc_data["atr_min"] is not None else float("nan")
            atr_max = acc_data["atr_max"] if acc_data["atr_max"] is not None else float("nan")
        if acc_data["rpm_cnt"] > 0:
            rpm_med = acc_data["rpm_sum"] / float(acc_data["rpm_cnt"])
        return _append_turn_point(turn_n, atr_med, atr_min, atr_max, rpm_med)

    # Aguarda ambos arquivos existirem.
    t0 = time.time()
    while _is_running() and _run_token_alive(run_token) and (not os.path.exists(dlg_csv_path) or not os.path.exists(drive_csv_path)):
        if time.time() - t0 > 15:
            log_msg("Distancia RT: timeout aguardando dlg.csv/drive.csv.")
            return
        time.sleep(0.05)

    # Arbitro de fonte:
    # - prioriza stream DIST/TURN vindo do agregador externo.
    # - ativa fallback local apenas se stream nao aparecer ou travar.
    t_gate = time.time()
    while _is_running() and _run_token_alive(run_token):
        with turn_rt_lock:
            mode = turn_rt_mode
            has_data = turn_rt_stream_has_data
            last_ts = turn_rt_stream_last_ts
            closed = turn_rt_stream_closed
        if mode == "fallback":
            break
        now = time.time()
        if has_data:
            if (now - last_ts) > TURN_RT_STREAM_STALL_S:
                _turn_rt_switch_to_fallback(
                    f"stream stall > {TURN_RT_STREAM_STALL_S:.1f}s"
                )
                break
        else:
            if closed:
                _turn_rt_switch_to_fallback("stream encerrou sem dados")
                break
            if (now - t_gate) > TURN_RT_STARTUP_GRACE_S:
                _turn_rt_switch_to_fallback(
                    f"sem ponto inicial de stream em {TURN_RT_STARTUP_GRACE_S:.1f}s"
                )
                break
        time.sleep(0.05)

    if _turn_rt_get_mode() != "fallback":
        return

    first_dist_logged = False
    first_turn_logged = False
    acc_dist = _acc_reset()
    acc_turn = _acc_reset()
    interval_idx = 1
    next_boundary_mm = dist_interval_mm
    next_boundary_turn = 1.0
    cum_dist_mm = 0.0
    cum_turn = 0.0
    prev_pos = 0.0
    prev_pos_valid = False
    prev_t_s = 0.0
    prev_t_valid = False
    dir_sign = 1

    have_d = False
    have_r = False
    dcols = None
    rcols = None

    try:
        with open(dlg_csv_path, "r", encoding="utf-8") as fdlg, \
             open(drive_csv_path, "r", encoding="utf-8") as fdrv:

            hd = fdlg.readline()
            hr = fdrv.readline()
            if hd and "idx" not in hd.lower():
                fdlg.seek(0)
            if hr and "idx" not in hr.lower():
                fdrv.seek(0)

            while _is_running() and _run_token_alive(run_token):
                if not have_d:
                    ld, complete_d = _read_complete_tail_line(fdlg)
                    if complete_d:
                        dcols = ld.strip().split(",")
                        have_d = True
                    else:
                        try:
                            fdlg.seek(fdlg.tell())
                        except Exception:
                            pass
                if not have_r:
                    lr, complete_r = _read_complete_tail_line(fdrv)
                    if complete_r:
                        rcols = lr.strip().split(",")
                        have_r = True
                    else:
                        try:
                            fdrv.seek(fdrv.tell())
                        except Exception:
                            pass

                if not have_d or not have_r:
                    time.sleep(0.01)
                    continue

                try:
                    idx_d = int(dcols[0])
                    idx_r = int(rcols[0])
                except Exception:
                    have_d = False
                    have_r = False
                    continue

                if idx_d < idx_r:
                    have_d = False
                    continue
                if idx_r < idx_d:
                    have_r = False
                    continue

                # idx alinhado
                have_d = False
                have_r = False

                # DLG parse
                dlg_err = 1
                try:
                    dlg_err = int(dcols[-1].strip())
                except Exception:
                    dlg_err = 1

                atr_ok = False
                atr = float("nan")
                if dlg_err == 0 and len(dcols) >= 12:
                    try:
                        atr = float(dcols[11])
                        atr_ok = True
                    except Exception:
                        atr_ok = False

                # Drive parse
                pos_ok = False
                rpm_ok = False
                pos = 0.0
                rpm = 0.0
                t_s_drv = float("nan")
                pos_mod = 65536.0
                try:
                    if len(rcols) > 2:
                        t_s_drv = float(rcols[2])
                except Exception:
                    t_s_drv = float("nan")
                try:
                    if len(rcols) > 5 and int(rcols[5]) == 0:
                        pos = float(rcols[3])
                        pos_ok = True
                except Exception:
                    pos_ok = False
                try:
                    if len(rcols) > 6 and int(rcols[6]) == 0:
                        rpm = float(rcols[4])
                        rpm_ok = True
                except Exception:
                    rpm_ok = False
                try:
                    if len(rcols) > 7:
                        parsed_mod = float(rcols[7])
                        if parsed_mod > 1.0:
                            pos_mod = parsed_mod
                except Exception:
                    pass

                # Acumulo dos agregadores do grafico 3.
                acc_dist["n_total"] += 1
                acc_turn["n_total"] += 1
                if atr_ok and pos_ok:
                    acc_dist["n_valid"] += 1
                    acc_turn["n_valid"] += 1
                    acc_dist["atr_sum"] += atr
                    acc_turn["atr_sum"] += atr
                    if acc_dist["atr_min"] is None or atr < acc_dist["atr_min"]:
                        acc_dist["atr_min"] = atr
                    if acc_turn["atr_min"] is None or atr < acc_turn["atr_min"]:
                        acc_turn["atr_min"] = atr
                    if acc_dist["atr_max"] is None or atr > acc_dist["atr_max"]:
                        acc_dist["atr_max"] = atr
                    if acc_turn["atr_max"] is None or atr > acc_turn["atr_max"]:
                        acc_turn["atr_max"] = atr
                    if rpm_ok:
                        acc_dist["rpm_sum"] += rpm
                        acc_turn["rpm_sum"] += rpm
                        acc_dist["rpm_cnt"] += 1
                        acc_turn["rpm_cnt"] += 1
                else:
                    acc_dist["n_fail"] += 1
                    acc_turn["n_fail"] += 1

                if pos_ok and prev_pos_valid and pos_mod > 1.0:
                    raw_diff = pos - prev_pos
                    d_eff = 0.0
                    if rpm_ok and abs(rpm) >= rpm_dir_deadband:
                        dir_sign = 1 if rpm >= 0.0 else -1

                    if dir_sign >= 0:
                        d_eff = raw_diff
                        if d_eff < (-0.5 * pos_mod):
                            d_eff += pos_mod
                        elif d_eff < 0.0:
                            d_eff = 0.0
                    else:
                        d_eff = raw_diff
                        if d_eff > (0.5 * pos_mod):
                            d_eff -= pos_mod
                        elif d_eff > 0.0:
                            d_eff = 0.0

                    if d_eff != 0.0:
                        dt_s = 0.0
                        if prev_t_valid and not np.isnan(t_s_drv):
                            dt_s = t_s_drv - prev_t_s
                            if dt_s <= 0.0 or dt_s > 1.0:
                                dt_s = 0.0
                        if rpm_ok and dt_s > 0.0:
                            max_step = pos_mod * (abs(rpm) / 60.0) * dt_s * 3.0 + 5.0
                        elif rpm_ok:
                            max_step = pos_mod * (abs(rpm) / 60.0) * 0.05 * 3.0 + 5.0
                        else:
                            max_step = pos_mod * 0.05
                        if abs(d_eff) > max_step:
                            d_eff = 0.0

                    motor_turn_inc = abs(d_eff) / (pos_mod * cycles_per_motor_rev)
                    pin_turn_inc = motor_turn_inc / relacao_mecanica
                    dist_inc_mm = pin_turn_inc * (2.0 * math.pi * raio_mm)
                    if pin_turn_inc > 0.0:
                        cum_turn += pin_turn_inc
                    if dist_inc_mm > 0.0:
                        cum_dist_mm += dist_inc_mm

                    while cum_dist_mm >= next_boundary_mm:
                        if _turn_rt_get_mode() == "fallback":
                            if _emit_dist_point(float(next_boundary_mm), acc_dist) and not first_dist_logged:
                                log_msg("Distancia RT: primeiro intervalo recebido para o grafico 3.")
                                first_dist_logged = True
                        interval_idx += 1
                        next_boundary_mm = float(interval_idx) * dist_interval_mm
                        acc_dist = _acc_reset()

                    while cum_turn >= next_boundary_turn:
                        if _turn_rt_get_mode() == "fallback":
                            if _emit_turn_point(float(next_boundary_turn), acc_turn) and not first_turn_logged:
                                log_msg("Volta RT: primeira volta recebida para o grafico 3.")
                                first_turn_logged = True
                        next_boundary_turn += 1.0
                        acc_turn = _acc_reset()

                if pos_ok:
                    prev_pos = pos
                    prev_pos_valid = True
                    if not np.isnan(t_s_drv):
                        prev_t_s = t_s_drv
                        prev_t_valid = True
    except Exception as e:
        log_msg(f"Distancia RT: erro no agregador em tempo real ({e}).")
# start_acquisition():
# - Funcao central de inicio de ensaio.
# - Etapas principais:
#   1) valida campos de formulario e tabela de etapas.
#   2) prepara pasta/arquivos de saida.
#   3) monta schedule (rpm, duracao_s).
#   4) inicia pipeline externo (padrao) via orch.start_external_run(...)
#      ou fluxo da versao anterior (go/go_p/velocidades) quando externo estiver desativado.
# - Side effects:
#   atualiza labels de estado, cronometro, threads de merge e plot.
def start_acquisition():

    global running
    # NOTE: declare globals before any assignment inside this function to avoid
    # Python "assigned before global declaration" errors (PyInstaller parse).
    global start_time, is_paused, tempo_pause_inicio, p_dist_target_mm, p_turn_target
    global p_strokes, p_atrito_ef, p_atrito_max, p_atrito_min, p_coluna_velocidade
    global p_turn_strokes, p_turn_atrito_ef, p_turn_atrito_max, p_turn_atrito_min, p_turn_coluna_velocidade
    global contador_amostras_total
    global x_auto_total_min, x3_auto_total_mm, x_time_ref_s

    if monitor_mode_active:
        messagebox.showwarning(
            "Monitoramento ativo",
            "Desative o modo Monitoramento antes de iniciar um ensaio."
        )
        return

    if tara_running:
        messagebox.showwarning(
            "Tara em andamento",
            "Aguarde a coleta de tara terminar antes de iniciar um novo ensaio."
        )
        return

    if run_finalizing or _external_pipeline_ativo():
        messagebox.showwarning(
            "Ensaio finalizando",
            "Aguarde o fechamento dos processos e arquivos do ensaio anterior antes de iniciar outro."
        )
        return

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
        

### FIM DAS VALIDACOES
###########################################################################################################################
    ####### INICIO DO PROGRAMA - GERACAO DA PASTA E ARQUIVOS TXT

    # Reseta a variavel antes de chamar (garante que nao pegue lixo de memoria)
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

    # Se o usuario clicou em "Cancelar" na janela de salvar, a variavel continuara vazia.
    # Se estiver vazia, damos um 'return' para cancelar o inicio da aquisicao.
    if not caminho_arquivo_1 or not caminho_arquivo_2:
        print("Inicio cancelado: Nenhum local escolhido para salvar.")
        return

    # Antes de iniciar cada ensaio, executa tara automatica obrigatoria.
    if not _run_auto_tara_before_start(repo_root, exe_info):
        _cleanup_run_folder_on_abort()
        log_msg("Inicio cancelado: tara automatica nao concluida.")
        return

    # Carrega os dados de calibracao CH1 apos a tara, para registrar
    # os valores efetivamente usados neste ensaio.
    ch1_fit = _load_ch1_fit_data(repo_root, exe_info.get("dlg_exe"))
    
    try:
        # -----------------------------------------------------------------------------
        # MUDANCA CONSCIENTE: metadados agora em CSV (info_ensaio.csv) com 2-3 colunas.
        # Formato:
        #   campo,valor,valor2
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
            f.write(f"Relacao mecanica (i),{RELACAO_MECANICA},\n")
            f.write(f"Intervalo distancia [mm],{DIST_INTERVAL_MM},\n")
            coluna_forca = float(ent_forca.get().strip().replace(',','.'))
            f.write(f"Força normal [N],{coluna_forca},\n")
            f.write("Dados de calibração,Fit CH1,\n")
            if ch1_fit:
                f.write(f"Slope (fit),{ch1_fit['slope']:.10g},\n")
                f.write(f"Intercept (fit),{ch1_fit['intercept']:.10g},\n")
                f.write(f"R2 (fit),{ch1_fit['r2']:.10g},\n")
            else:
                f.write("Slope (fit),NULL,\n")
                f.write("Intercept (fit),NULL,\n")
                f.write("R2 (fit),NULL,\n")

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
        messagebox.showerror("Erro de Arquivo", f"Nao foi possivel gravar o arquivo de Entrada\n{e}")
        return
    
    ########## PREENCHER AS LISTAS COM AS VELOCIDADES E DURACAO OBTIDAS
    lista_velocidades_digitadas = []
    lista_duracao = []
    # Percorre os 11 itens da interface
    for i in range(11):
        
        # --- PARTE DA VELOCIDADE (Vem de um Entry -> usa .get()) ---
        texto_vel = lista_entries_velocidade[i].get().strip()
        
        # --- PARTE DA DURACAO (Vem de um Label -> usa .cget("text")) ---
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
    global cof_force_normal_n
    try:
        cof_force_normal_n = float(ent_forca.get().strip().replace(',', '.'))
    except Exception:
        cof_force_normal_n = 0.0

    if USE_EXTERNAL_RUNNER:
        try:
            raio_mm = float(ent_raio.get().strip().replace(',', '.'))
        except Exception:
            messagebox.showwarning("Valor Inválido", "Raio inválido para converter velocidade.")
            return
        try:
            forca_normal_n = float(ent_forca.get().strip().replace(',', '.'))
        except Exception:
            messagebox.showwarning("Valor Inválido", "Força normal inválida para calcular atrito.")
            return

        # Monta schedule (rpm, duracao_s)
        schedule = []
        for vel_mm_s, dur_s in zip(lista_velocidades_digitadas, lista_duracao):
            # Entrada: velocidade linear (mm/s), raio (mm), relacao mecanica.
            # Saida: setpoint inteiro em rpm para o logger do Drive.
            rpm = orch.rpm_from_mm_s(vel_mm_s, raio_mm, RELACAO_MECANICA)
            schedule.append((rpm, dur_s))
        rpm_schedule = [rpm for rpm, _ in schedule]

        if not schedule:
            messagebox.showwarning("Tabela vazia", "Nenhuma etapa válida para iniciar o ensaio.")
            return

        # Reseta buffers dos graficos para o novo ensaio.
        sampsTimestamp.clear()
        for i in range(numChannels):
            graSamps[i].clear()
        p_strokes.clear()
        p_atrito_ef.clear()
        p_atrito_max.clear()
        p_atrito_min.clear()
        p_coluna_velocidade.clear()
        p_turn_strokes.clear()
        p_turn_atrito_ef.clear()
        p_turn_atrito_max.clear()
        p_turn_atrito_min.clear()
        p_turn_coluna_velocidade.clear()
        x_time_ref_s = None
        # Define alvo de distancia para eixo X do grafico 3.
        p_dist_target_mm = _calc_expected_distance_mm_from_table()
        p_turn_target = _calc_expected_pin_turns_from_table(raio_mm)
        if p_dist_target_mm > 0:
            x3_auto_total_mm = p_dist_target_mm

        dur_total = sum(d for _, d in schedule)
        # Em modo automatico, eixo X de Temperatura/CoF segue duracao total prevista.
        if dur_total > 0:
            x_auto_total_min = float(dur_total) / 60.0
        _bump_graph_config_revision()
        rate_hz = float(getattr(orch, "DEFAULT_RATE_HZ", 50.0))

        # Caminhos padrao (gerados por salvar_arquivo)
        out_paths = {
            "dlg_csv": caminho_dlg_csv,
            "drive_csv": caminho_drive_csv,
            "turn_dist_csv": caminho_turn_dist_csv,
            "turn_vp_csv": caminho_turn_volta_csv,
            "merge_csv": caminho_merge_csv,
            "schedule_csv": caminho_schedule_csv,
        }

        # Dispara os executaveis em background.
        # start_external_run(...) recebe:
        # - repo_root: raiz do projeto para localizar .exe.
        # - out_paths: caminhos de saida (dlg/drive/merge/schedule).
        # - schedule: lista [(rpm, duracao_s), ...] para o drive.
        # - duration_s/rate_hz: janela total e taxa-alvo de amostragem.
        # - bind_port/com_port/slave/baud/parity: parametros de comunicacao.
        #
        # Retorno esperado:
        # - objeto RunState com handles de processo (dlg_proc/drive_proc)
        #   e caminhos usados durante o ensaio.
        repo_root = orch.find_repo_root()
        global external_run_state
        try:
            external_run_state = orch.start_external_run(
                repo_root=repo_root,
                out_paths=out_paths,
                schedule=schedule,
                duration_s=dur_total,
                rate_hz=rate_hz,
                # Usa a porta padrao do pipeline (41402).
                # bind_port=0 (efemera) pode gerar execucao sem ACQDATA em
                # setups onde o DLG responde de forma mais estavel na porta fixa.
                bind_port=getattr(orch, "DEFAULT_BIND_PORT", 41402),
                com_port="COM5",
                slave_id=1,
                baud=115200,
                parity="E",
                show_console=False,
                force_normal_n=forca_normal_n,
                # i = D2 / D1 (D1=motor, D2=disco), mesma definicao do supervisório.
                relacao=RELACAO_MECANICA,
                raio_mm=raio_mm,
                distance_interval_mm=DIST_INTERVAL_MM
            )
        except Exception as e:
            _set_run_finalizing(False)
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
        global external_run_token
        external_run_token += 1
        run_token = external_run_token
        # Modo atual: grafico 3 em tempo real 100% no Python.
        _turn_rt_reset_state("fallback")
        running = "true"
        _set_start_button_state()
        is_paused = False
        tempo_pause_inicio = 0
        graph_log(
            "RUN start token={0} rate_hz={1} dur_total_s={2:.3f} relacao={3:.6f} dist_target_mm={4:.6f} turn_target={5:.6f} interval_mm={6:.6f}".format(
                run_token, rate_hz, dur_total, RELACAO_MECANICA, p_dist_target_mm, p_turn_target, DIST_INTERVAL_MM
            )
        )
        try:
            label_ensaio_estado.config(text="Em andamento", fg="#0078D4")
            _set_targets_stopped()
        except Exception:
            pass
        if 'button_frame5_pausar' in globals():
            button_frame5_pausar.config(text="Pausar", bg="SystemButtonFace")

        # Relogio do ensaio:
        # - so inicia quando o DLG validar N amostras (evita tempo correr antes do DLG).
        global timer_started
        timer_started = False

        # Thread que aguarda fim e faz merge (nao bloqueia GUI).
        # orch.wait_and_merge(state_snapshot) bloqueia ate os dois processos
        # encerrarem, e depois gera resultado_ensaio.csv.
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
                global running, is_paused, external_run_state, tempo_pause_inicio, timer_started, graph_events_log_path
                _set_run_finalizing(True)
                # Apos o merge, os artefatos tecnicos vao para <pasta da execucao>\\DadosDev.
                # Atualiza o destino do graph_log para evitar recriar graph_events.log na raiz da execucao.
                try:
                    rep_dir = os.path.dirname(caminho_merge_csv)
                    dev_graph = os.path.join(rep_dir, "DadosDev", "graph_events.log")
                    if os.path.isdir(os.path.join(rep_dir, "DadosDev")):
                        graph_events_log_path = dev_graph
                except Exception:
                    pass
                running = False
                is_paused = False
                tempo_pause_inicio = 0
                timer_started = False
                external_run_state = None
                x_time_ref_s = None
                try:
                    # Segunda passada para garantir que dlg.csv/drive.csv
                    # sejam recortados para DadosDev apos fechamento das tails.
                    orch.finalize_dev_artifacts_after_run(state_snapshot)
                except Exception as e:
                    log_msg(f"Aviso: nao foi possivel finalizar move de CSVs tecnicos ({e}).")
                graph_log("RUN end token={0}".format(run_token))
                try:
                    label_ensaio_estado.config(text="Finalizado", fg="green")
                    _set_targets_stopped()
                except Exception:
                    pass
                try:
                    button_frame5_pausar.config(text="Pausar", bg="SystemButtonFace")
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
                        p_turn_strokes.clear()
                        p_turn_atrito_ef.clear()
                        p_turn_atrito_max.clear()
                        p_turn_atrito_min.clear()
                        p_turn_coluna_velocidade.clear()
                        globals()["x_time_ref_s"] = None
                        globals()["p_dist_target_mm"] = 0.0
                        globals()["p_turn_target"] = 0.0
                        _bump_graph_config_revision()
                        ax1.clear()
                        ax2.clear()
                        ax3.clear()
                        if ax3_twin is not None:
                            ax3_twin.clear()
                    except Exception:
                        pass
                try:
                    def _save_image_and_clear_graphs():
                        try:
                            _save_graficos_tab_image(caminho_graficos_png)
                        finally:
                            try:
                                _clear_graphs()
                            finally:
                                _set_run_finalizing(False)

                    root.after(0, _save_image_and_clear_graphs)
                except Exception:
                    _save_image_and_clear_graphs()
        threading.Thread(target=_wait_and_merge, daemon=True).start()

        # Thread para alimentar graficos em tempo real a partir do dlg.csv
        threading.Thread(
            target=_tail_dlg_csv_for_graphs,
            args=(caminho_dlg_csv, run_token),
            daemon=True
        ).start()
        # Grafico 3 em tempo real:
        # agrega por idx diretamente de dlg.csv+drive.csv no Python,
        # por intervalos de distancia configurados.
        threading.Thread(
            target=_tail_realtime_dist_from_logs,
            args=(caminho_dlg_csv, caminho_drive_csv, RELACAO_MECANICA, raio_mm, DIST_INTERVAL_MM, 1.0, run_token),
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
            args=(caminho_dlg_csv, 3, 15, run_token),
            daemon=True
        ).start()
        return

    ########## PARTE DOS GRÁFICOS
    global tensao_vel, duracao, soma_tempos_vel, tempo_total_aqc

    contador_amostras_total = 0

    # Limpar as listas do terceiro gráfico
    p_strokes.clear()
    p_atrito_ef.clear()
    p_atrito_max.clear()
    p_atrito_min.clear()
    p_turn_strokes.clear()
    p_turn_atrito_ef.clear()
    p_turn_atrito_max.clear()
    p_turn_atrito_min.clear()

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
    _bump_graph_config_revision()
        
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


# pause_acquisition():
# - Alterna entre pausado/rodando.
# - Modo externo: envia PAUSE/RESUME via IPC para drive e dlg.
# - Modo da versao anterior: congela contagem local e controle analogico.
def pause_acquisition():
    global is_paused, start_time, tempo_pause_inicio, running, external_run_state
    
    # Só funciona se a aquisição estiver rodando (running == "true")
    if running != "true":
        return

    if USE_EXTERNAL_RUNNER:
        if external_run_state is None:
            messagebox.showwarning("Pausar", "Ensaio externo não está ativo.")
            return

        if not is_paused:
            try:
                # pause_run(state) envia IPC:
                #   drive_proc <- "PAUSE\\n"
                #   dlg_proc   <- "PAUSE\\n"
                orch.pause_run(external_run_state)
            except Exception as e:
                messagebox.showerror("Erro ao pausar", f"Falha ao pausar ensaio externo.\n\n{e}")
                return
            is_paused = True
            tempo_pause_inicio = time.time()
            try:
                label_ensaio_estado.config(text="Pausado", fg="darkorange")
            except Exception:
                pass
            if 'button_frame5_pausar' in globals():
                button_frame5_pausar.config(text="Retomar", bg="yellow")
            log_msg("Ensaio pausado.")
            return

        try:
            # resume_run(state) envia IPC:
            #   dlg_proc   <- "RESUME\\n"
            #   drive_proc <- "RESUME\\n"
            orch.resume_run(external_run_state)
        except Exception as e:
            messagebox.showerror("Erro ao retomar", f"Falha ao retomar ensaio externo.\n\n{e}")
            return

        is_paused = False
        tempo_agora = time.time()
        if timer_started and tempo_pause_inicio > 0:
            start_time = start_time + (tempo_agora - tempo_pause_inicio)
        tempo_pause_inicio = 0
        try:
            label_ensaio_estado.config(text="Em andamento", fg="#0078D4")
        except Exception:
            pass
        if 'button_frame5_pausar' in globals():
            button_frame5_pausar.config(text="Pausar", bg="SystemButtonFace")
        log_msg("Ensaio retomado.")
        atualiza_decorrido()
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


# stop_acquisition():
# - Solicita confirmacao do usuario e encerra ensaio em seguranca.
# - Modo externo: envia STOP e aguarda fechamento/merge final.
# - Modo da versao anterior: zera tensao e limpa buffers/plots.
def stop_acquisition():
    global running, is_paused, ip, timer_started, tempo_pause_inicio, p_dist_target_mm, p_turn_target, x_time_ref_s
    global graSamps, sampsTimestamp, p_strokes, p_atrito_ef, p_atrito_max, p_atrito_min, p_coluna_velocidade
    global p_turn_strokes, p_turn_atrito_ef, p_turn_atrito_max, p_turn_atrito_min, p_turn_coluna_velocidade

    if running:
        resposta = messagebox.askyesno("Confirmação", "Deseja parar o programa?")
        if resposta:
            running = False
            is_paused = False
            tempo_pause_inicio = 0

            # --------------------------------------------------------------
            # INTEGRACAO COM LOGGERS EM C:
            # Se o modo externo estiver ativo, finalizamos os processos aqui.
            # --------------------------------------------------------------
            global external_run_state
            if USE_EXTERNAL_RUNNER and external_run_state is not None:
                state_snapshot = external_run_state
                external_run_state = None
                _set_run_finalizing(True)
                def _stop_external():
                    try:
                        # stop_run(state) tenta parada graciosa por IPC ("STOP"),
                        # aguarda flush/fechamento e so entao usa terminate/kill.
                        orch.stop_run(state_snapshot)
                    except Exception as e:
                        log_msg(f"Erro ao solicitar parada externa: {e}")
                threading.Thread(target=_stop_external, daemon=True).start()
                try:
                    label_ensaio_estado.config(text="Finalizando...", fg="darkorange")
                    _set_targets_stopped()
                    lbl_tempo_decorrido2.config(text="0:00:00")
                except Exception:
                    pass
                timer_started = False
                if 'button_frame5_pausar' in globals():
                    button_frame5_pausar.config(text="Pausar", bg="SystemButtonFace")
                log_msg("Parada solicitada. Finalizando arquivos e merge...")
                return

            if 'button_frame5_pausar' in globals():
                button_frame5_pausar.config(text="Pausar", bg="SystemButtonFace")

            lbl_tempo_decorrido2.config(text="0:00:00")
            try:
                label_ensaio_estado.config(text="Aguardando novo ensaio", fg="black")
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
            p_turn_strokes.clear()
            p_turn_atrito_ef.clear()
            p_turn_atrito_max.clear()
            p_turn_atrito_min.clear()
            p_turn_coluna_velocidade.clear()
            p_dist_target_mm = 0.0
            p_turn_target = 0.0
            x_time_ref_s = None
            _bump_graph_config_revision()

            # Limpa os eixos imediatamente
            ax1.clear()

# Nicolas
# update1(frame): Limpa o ax1, verifica os canais ativos, processa o tempo baseada na frequência e plota o gráfico (ax1.plot) da temperatura.
# Ela é chamada pelo agendador Tk dos graficos, somente quando a coluna lateral esta visivel.
# update1(frame):
# - Renderiza grafico 1 (temperatura) usando CH2 do DLG.
# - Aplica escala X compartilhada (automatico por duracao total ou manual ciclico).
# - Respeita configuracao de eixo Y automatico/manual.
def _should_plot_graph_series():
    return _is_running() or monitor_mode_active or bool(globals().get("graph_snapshot_rendering", False))


def update1(frame):
    # Acessa a lista de tempos reais
    global sampsTimestamp 

    try:
        # Apaga tudo o que foi desenhado no quadro anterior para desenhar o novo.
        ax1.clear()

        # Regras fixas (validacao):
        # - Grafico 1 (temperatura) usa CH2 do DLG.
        # Mantemos os checkboxes, mas este grafico respeita apenas o canal 2.
        c2 = canalativo2.get()

        # Janela X compartilhada (automatico ou manual ciclico).
        limite_tela_min = _current_x_window_min()
        passo = 5 # Desenha só 1 ponto a cada 5. Deixa o programa mais leve

        # Funcao auxiliar para preparar e plotar a serie temporal conforme modo X.
        def plotar_canal(idx, cor, label):
            if len(graSamps) > idx:
                raw_dados = graSamps[idx]
                # Garante que nao vai acessar indices inexistentes
                tam = min(len(sampsTimestamp), len(raw_dados))
                
                if tam > 0:
                    # Pega os dados sincronizados, cortando as listas para ficarem do mesmo tamanho
                    t_data = np.array(sampsTimestamp[:tam])
                    y_data = np.array(raw_dados[:tam])
                    
                    if len(t_data) > 0:
                        tempo, dados = _x_plot_series(t_data, y_data)
                        tempo = tempo[::passo]
                        dados = dados[::passo]
                        tempo, dados = _downsample_for_plot(tempo, dados, max_points=1400)
                        if len(tempo) == 0:
                            return
                        # Manda desenhar a linha
                        if cor:
                            ax1.plot(tempo, dados, label=label, color=cor)
                        else:
                            ax1.plot(tempo, dados, label=label)

        if _should_plot_graph_series() and graSamps and c2:
            # CH2 (temperatura) = index 1 na lista graSamps.
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
    except Exception as e:
        log_msg(f"Grafico 1: erro de atualizacao ({e}).")

# update2(frame):
# - Renderiza grafico 2 em CoF usando CH1 do DLG.
# - Usa mesmo eixo temporal do grafico 1 para alinhamento visual.
# - Respeita configuracao de eixo Y automatico/manual.
def update2(frame):
    global sampsTimestamp
    try:
        ax2.clear()
        # Regras fixas (validacao):
        # - Grafico 2 usa CH1 convertido em CoF.
        c1 = canalativo1.get()
        limite_tela_min = _current_x_window_min()
        passo = 5

        # Canal 1 (CoF)
        if _should_plot_graph_series() and graSamps and c1 == 1 and len(graSamps) > 0:
            # Pega a lista bruta de CH1 (forca) para converter em CoF.
            raw_dados = graSamps[0]
            tam = min(len(sampsTimestamp), len(raw_dados))
            
            if tam > 0:
                t_data = np.array(sampsTimestamp[:tam])
                y_force = np.array(raw_dados[:tam], dtype=float)
                fn = cof_force_normal_n
                if fn is None or fn <= 0.0:
                    # Fallback para manter o grafico funcional fora de ensaio.
                    try:
                        fn = float(ent_forca.get().strip().replace(',', '.'))
                    except Exception:
                        fn = 0.0
                if fn <= 0.0:
                    y_data = np.full_like(y_force, np.nan)
                else:
                    y_data = y_force / fn

                if len(t_data) > 0:
                    tempo, dados = _x_plot_series(t_data, y_data)
                    tempo = tempo[::passo]
                    dados = dados[::passo]
                    tempo, dados = _downsample_for_plot(tempo, dados, max_points=1400)
                    if len(tempo) > 0:
                        ax2.plot(tempo, dados, label='CoF', color='#1f77b4') # Desenha a linha azul

        # Configuração Visual Ax2
        handles, labels = ax2.get_legend_handles_labels()
        if handles: 
            ax2.legend(loc='upper right', fontsize='small')
        
        ax2.set_xlim(0, limite_tela_min)
        ax2.grid(alpha=0.5, lw=0.5)
        ax2.set_facecolor('black')
        ax2.tick_params(axis='both', labelsize=8)
        ax2.set_xlabel('Tempo [min]', fontsize=9)
        ax2.set_ylabel('CoF [-]', fontsize=9)
        
        if not y2_auto:
            ax2.set_ylim(y2_min, y2_max)
    except Exception as e:
        log_msg(f"Grafico 2: erro de atualizacao ({e}).")


# Nicolas
# Limpa o ax3 e plota atrito medio/min/max por distancia.
# update3(frame):
# - Renderiza grafico 3 por distancia (atrito medio/max/min por intervalo).
# - Eixo X segue distancia registrada, com limite previsto do ensaio.
# - Opera sobre listas p_strokes/p_atrito_* alimentadas pelos agregadores.
def update3(frame):
    """
    Atualiza o grafico 3 (atrito por distancia).
    Mantem labels corretos mesmo fora de ensaio para evitar voltar ao layout antigo.
    """
    try:
        global ax3_twin
        ax3.clear()
        if ax3_twin is None:
            ax3_twin = ax3.twinx()
        ax3_twin.clear()
        # Garante que o eixo secundario fique sempre do lado direito.
        ax3_twin.yaxis.set_label_position("right")
        ax3_twin.yaxis.tick_right()
        ax3_twin.spines["right"].set_visible(True)
        ax3_twin.spines["left"].set_visible(False)
        ax3_twin.spines["right"].set_position(("axes", 1.0))
        ax3_twin.patch.set_alpha(0.0)
        mode = graph3_mode_var.get() if "graph3_mode_var" in globals() else graph3_mode
        mode = str(mode).strip().lower()
        if mode not in ("dist", "turn"):
            mode = "dist"

        if mode == "turn":
            x_vals = np.array(p_turn_strokes, dtype=float) if len(p_turn_strokes) > 0 else np.array([])
            y_max = np.array(p_turn_atrito_max, dtype=float) if len(p_turn_atrito_max) > 0 else np.array([])
            y_med = np.array(p_turn_atrito_ef, dtype=float) if len(p_turn_atrito_ef) > 0 else np.array([])
            y_min = np.array(p_turn_atrito_min, dtype=float) if len(p_turn_atrito_min) > 0 else np.array([])
            rpm_vals = np.array(p_turn_coluna_velocidade, dtype=float) if len(p_turn_coluna_velocidade) > 0 else np.array([])
        else:
            x_vals = np.array(p_strokes, dtype=float) if len(p_strokes) > 0 else np.array([])
            y_max = np.array(p_atrito_max, dtype=float) if len(p_atrito_max) > 0 else np.array([])
            y_med = np.array(p_atrito_ef, dtype=float) if len(p_atrito_ef) > 0 else np.array([])
            y_min = np.array(p_atrito_min, dtype=float) if len(p_atrito_min) > 0 else np.array([])
            rpm_vals = np.array(p_coluna_velocidade, dtype=float) if len(p_coluna_velocidade) > 0 else np.array([])

        vel_plot = np.array([])

        if len(x_vals) > 0:
            if x3_auto:
                x_plot = x_vals
                y_max_plot = y_max
                y_med_plot = y_med
                y_min_plot = y_min
                rpm_plot = rpm_vals
            else:
                span_mm = max(0.001, float(x3_manual_mm))
                if mode == "turn":
                    try:
                        raio_mm = float(ent_raio.get().strip().replace(",", "."))
                    except Exception:
                        raio_mm = 0.0
                    if raio_mm > 0:
                        span_mm = max(0.001, span_mm / (2.0 * math.pi * raio_mm))
                ciclos = np.floor(np.maximum(x_vals, 0.0) / span_mm)
                ciclo_atual = np.max(ciclos) if len(ciclos) > 0 else 0.0
                mask = (ciclos == ciclo_atual)
                x_plot = x_vals[mask] - (ciclo_atual * span_mm)
                y_max_plot = y_max[mask]
                y_med_plot = y_med[mask]
                y_min_plot = y_min[mask]
                rpm_plot = rpm_vals[mask]

            try:
                raio_mm = float(ent_raio.get().strip().replace(",", "."))
            except Exception:
                raio_mm = 0.0
            vel_plot = _rpm_motor_to_pin_speed_mm_s(rpm_plot, raio_mm, RELACAO_MECANICA)
            x_plot, y_max_plot, y_med_plot, y_min_plot, vel_plot = _downsample_for_plot(
                x_plot, y_max_plot, y_med_plot, y_min_plot, vel_plot, max_points=1200
            )
            ax3.plot(x_plot, y_max_plot, color='red', label='Atrito max', linewidth=1)
            ax3.plot(x_plot, y_med_plot, color='black', label='Atrito medio', linewidth=1)
            ax3.plot(x_plot, y_min_plot, color='cyan', label='Atrito min', linewidth=1)
            if len(vel_plot) == len(x_plot) and len(vel_plot) > 0:
                valid_v = np.isfinite(vel_plot)
                if np.any(valid_v):
                    ax3_twin.plot(
                        x_plot[valid_v],
                        vel_plot[valid_v],
                        color='darkorange',
                        linestyle='--',
                        linewidth=1,
                        label='Velocidade media'
                    )

        if x3_auto:
            if mode == "turn":
                x_target = max(1.0, float(p_turn_target) if p_turn_target > 0 else 1.0)
                if not _is_running():
                    try:
                        raio_mm = float(ent_raio.get().strip().replace(",", "."))
                    except Exception:
                        raio_mm = 0.0
                    previsto_turn = _calc_expected_pin_turns_from_table(raio_mm)
                    if previsto_turn > 0:
                        x_target = max(x_target, float(previsto_turn))
            else:
                x_target = max(
                    1.0,
                    float(p_dist_target_mm) if p_dist_target_mm > 0 else 1.0,
                    float(x3_auto_total_mm) if x3_auto_total_mm > 0 else 1.0,
                )
                if not _is_running():
                    previsto_mm = _calc_expected_distance_mm_from_table()
                    if previsto_mm > 0:
                        x_target = max(x_target, float(previsto_mm))
            if len(x_vals) > 0:
                if mode == "turn":
                    x_target = max(x_target, float(x_vals[-1]))
                else:
                    x_target = max(x_target, float(x_vals[-1]))
        else:
            if mode == "turn":
                try:
                    raio_mm = float(ent_raio.get().strip().replace(",", "."))
                except Exception:
                    raio_mm = 0.0
                if raio_mm > 0:
                    x_target = max(1.0, float(x3_manual_mm) / (2.0 * math.pi * raio_mm))
                else:
                    x_target = max(1.0, float(x3_manual_mm))
            else:
                x_target = max(1.0, float(x3_manual_mm))
        ax3.set_xlim(0, x_target)

        if mode == "turn":
            ax3.set_xlabel('Voltas (pino)', fontsize=9)
        else:
            ax3.set_xlabel('Distancia [mm]', fontsize=9)
        ax3.set_ylabel('Atrito [-]', fontsize=9)
        ax3.grid(True, alpha=0.5)
        ax3.tick_params(axis='both', labelsize=8)
        ax3.set_facecolor('white')
        ax3_twin.set_ylabel('Velocidade [mm/s]', fontsize=9, color='darkorange')
        ax3_twin.yaxis.set_label_coords(1.10, 0.5)
        ax3_twin.tick_params(axis='y', labelsize=8, colors='darkorange')
        ax3_twin.grid(False)
        if len(vel_plot) > 0 and np.any(np.isfinite(vel_plot)):
            vmax = np.nanmax(vel_plot)
            if np.isfinite(vmax):
                ax3_twin.set_ylim(0, max(1.0, float(vmax) * 1.15))
        if not y3_auto:
            ax3.set_ylim(y3_min, y3_max)

        h1, l1 = ax3.get_legend_handles_labels()
        h2, l2 = ax3_twin.get_legend_handles_labels()
        handles = h1 + h2
        labels = l1 + l2
        if handles:
            # Legenda unificada horizontal abaixo do G3 para nao cobrir elementos.
            ax3.legend(
                handles, labels,
                loc='upper center',
                bbox_to_anchor=(0.5, -0.20),
                borderaxespad=0.0,
                fontsize='small',
                ncol=4
            )
    except Exception as e:
        log_msg(f"Grafico 3: erro de atualizacao ({e}).")


def _plot_temp_axis_view(ax):
    """
    Desenha temperatura (CH2) em um eixo fornecido, usando as mesmas
    configuracoes de X/Y do grafico 1 da aba principal.
    """
    ax.clear()
    c2 = canalativo2.get()
    limite_tela_min = _current_x_window_min()
    passo = 5

    if _should_plot_graph_series() and graSamps and c2 and len(graSamps) > 1:
        raw_dados = graSamps[1]
        tam = min(len(sampsTimestamp), len(raw_dados))
        if tam > 0:
            t_data = np.array(sampsTimestamp[:tam])
            y_data = np.array(raw_dados[:tam])
            if len(t_data) > 0:
                tempo, dados = _x_plot_series(t_data, y_data)
                tempo = tempo[::passo]
                dados = dados[::passo]
                tempo, dados = _downsample_for_plot(tempo, dados, max_points=1100)
                if len(tempo) > 0:
                    ax.plot(tempo, dados, label='Channel 2')

    if ax.get_legend_handles_labels()[0]:
        ax.legend(loc='upper right', fontsize='x-small')
    ax.set_xlim(0, limite_tela_min)
    ax.grid(alpha=0.5, lw=0.5)
    ax.set_facecolor('black')
    ax.tick_params(axis='both', labelsize=8)
    ax.set_xlabel('Tempo [min]', fontsize=9)
    ax.set_ylabel('Temperatura [°C]', fontsize=9)
    if not y1_auto:
        ax.set_ylim(y1_min, y1_max)


def _plot_cof_axis_view(ax):
    """
    Desenha CoF (CH1/forca normal) em um eixo fornecido, usando as mesmas
    configuracoes de X/Y do grafico 2 da aba principal.
    """
    ax.clear()
    c1 = canalativo1.get()
    limite_tela_min = _current_x_window_min()
    passo = 5

    if _should_plot_graph_series() and graSamps and c1 == 1 and len(graSamps) > 0:
        raw_dados = graSamps[0]
        tam = min(len(sampsTimestamp), len(raw_dados))
        if tam > 0:
            t_data = np.array(sampsTimestamp[:tam])
            y_force = np.array(raw_dados[:tam], dtype=float)
            fn = cof_force_normal_n
            if fn is None or fn <= 0.0:
                try:
                    fn = float(ent_forca.get().strip().replace(',', '.'))
                except Exception:
                    fn = 0.0
            if fn <= 0.0:
                y_data = np.full_like(y_force, np.nan)
            else:
                y_data = y_force / fn

            if len(t_data) > 0:
                tempo, dados = _x_plot_series(t_data, y_data)
                tempo = tempo[::passo]
                dados = dados[::passo]
                tempo, dados = _downsample_for_plot(tempo, dados, max_points=1100)
                if len(tempo) > 0:
                    ax.plot(tempo, dados, label='CoF', color='#1f77b4')

    if ax.get_legend_handles_labels()[0]:
        ax.legend(loc='upper right', fontsize='x-small')
    ax.set_xlim(0, limite_tela_min)
    ax.grid(alpha=0.5, lw=0.5)
    ax.set_facecolor('black')
    ax.tick_params(axis='both', labelsize=8)
    ax.set_xlabel('Tempo [min]', fontsize=9)
    ax.set_ylabel('CoF [-]', fontsize=9)
    if not y2_auto:
        ax.set_ylim(y2_min, y2_max)


def _plot_g3_mode_axis_view(ax, mode, twin_ax=None):
    """
    Desenha atrito agregado no eixo informado para um modo fixo:
    - mode='dist': processamento por distancia.
    - mode='turn': processamento por volta.

    Usa as mesmas configuracoes de eixo X/Y do grafico 3 principal.
    """
    ax.clear()
    if twin_ax is not None:
        twin_ax.clear()
        twin_ax.yaxis.set_label_position("right")
        twin_ax.yaxis.tick_right()
        twin_ax.spines["right"].set_visible(True)
        twin_ax.spines["left"].set_visible(False)
        twin_ax.spines["right"].set_position(("axes", 1.0))
        twin_ax.patch.set_alpha(0.0)
    mode = str(mode).strip().lower()
    if mode not in ("dist", "turn"):
        mode = "dist"

    if mode == "turn":
        x_vals = np.array(p_turn_strokes, dtype=float) if len(p_turn_strokes) > 0 else np.array([])
        y_max = np.array(p_turn_atrito_max, dtype=float) if len(p_turn_atrito_max) > 0 else np.array([])
        y_med = np.array(p_turn_atrito_ef, dtype=float) if len(p_turn_atrito_ef) > 0 else np.array([])
        y_min = np.array(p_turn_atrito_min, dtype=float) if len(p_turn_atrito_min) > 0 else np.array([])
        rpm_vals = np.array(p_turn_coluna_velocidade, dtype=float) if len(p_turn_coluna_velocidade) > 0 else np.array([])
    else:
        x_vals = np.array(p_strokes, dtype=float) if len(p_strokes) > 0 else np.array([])
        y_max = np.array(p_atrito_max, dtype=float) if len(p_atrito_max) > 0 else np.array([])
        y_med = np.array(p_atrito_ef, dtype=float) if len(p_atrito_ef) > 0 else np.array([])
        y_min = np.array(p_atrito_min, dtype=float) if len(p_atrito_min) > 0 else np.array([])
        rpm_vals = np.array(p_coluna_velocidade, dtype=float) if len(p_coluna_velocidade) > 0 else np.array([])

    vel_plot = np.array([])
    x_plot = np.array([])
    rpm_plot = np.array([])

    if len(x_vals) > 0:
        if x3_auto:
            x_plot = x_vals
            y_max_plot = y_max
            y_med_plot = y_med
            y_min_plot = y_min
            rpm_plot = rpm_vals
        else:
            span_mm = max(0.001, float(x3_manual_mm))
            if mode == "turn":
                try:
                    raio_mm = float(ent_raio.get().strip().replace(",", "."))
                except Exception:
                    raio_mm = 0.0
                if raio_mm > 0:
                    span_mm = max(0.001, span_mm / (2.0 * math.pi * raio_mm))
            ciclos = np.floor(np.maximum(x_vals, 0.0) / span_mm)
            ciclo_atual = np.max(ciclos) if len(ciclos) > 0 else 0.0
            mask = (ciclos == ciclo_atual)
            x_plot = x_vals[mask] - (ciclo_atual * span_mm)
            y_max_plot = y_max[mask]
            y_med_plot = y_med[mask]
            y_min_plot = y_min[mask]
            rpm_plot = rpm_vals[mask]

        if twin_ax is not None:
            try:
                raio_mm = float(ent_raio.get().strip().replace(",", "."))
            except Exception:
                raio_mm = 0.0
            vel_plot = _rpm_motor_to_pin_speed_mm_s(rpm_plot, raio_mm, RELACAO_MECANICA)
            x_plot, y_max_plot, y_med_plot, y_min_plot, vel_plot = _downsample_for_plot(
                x_plot, y_max_plot, y_med_plot, y_min_plot, vel_plot, max_points=900
            )
        else:
            x_plot, y_max_plot, y_med_plot, y_min_plot = _downsample_for_plot(
                x_plot, y_max_plot, y_med_plot, y_min_plot, max_points=900
            )

        ax.plot(x_plot, y_max_plot, color='red', label='Atrito max', linewidth=1)
        ax.plot(x_plot, y_med_plot, color='black', label='Atrito medio', linewidth=1)
        ax.plot(x_plot, y_min_plot, color='cyan', label='Atrito min', linewidth=1)

        if twin_ax is not None:
            if len(vel_plot) == len(x_plot) and len(vel_plot) > 0:
                valid_v = np.isfinite(vel_plot)
                if np.any(valid_v):
                    twin_ax.plot(
                        x_plot[valid_v],
                        vel_plot[valid_v],
                        color='darkorange',
                        linestyle='--',
                        linewidth=1,
                        label='Velocidade media'
                    )

    if x3_auto:
        if mode == "turn":
            x_target = max(1.0, float(p_turn_target) if p_turn_target > 0 else 1.0)
            if not _is_running():
                try:
                    raio_mm = float(ent_raio.get().strip().replace(",", "."))
                except Exception:
                    raio_mm = 0.0
                previsto_turn = _calc_expected_pin_turns_from_table(raio_mm)
                if previsto_turn > 0:
                    x_target = max(x_target, float(previsto_turn))
        else:
            x_target = max(
                1.0,
                float(p_dist_target_mm) if p_dist_target_mm > 0 else 1.0,
                float(x3_auto_total_mm) if x3_auto_total_mm > 0 else 1.0,
            )
            if not _is_running():
                previsto_mm = _calc_expected_distance_mm_from_table()
                if previsto_mm > 0:
                    x_target = max(x_target, float(previsto_mm))
        if len(x_vals) > 0:
            x_target = max(x_target, float(x_vals[-1]))
    else:
        if mode == "turn":
            try:
                raio_mm = float(ent_raio.get().strip().replace(",", "."))
            except Exception:
                raio_mm = 0.0
            if raio_mm > 0:
                x_target = max(1.0, float(x3_manual_mm) / (2.0 * math.pi * raio_mm))
            else:
                x_target = max(1.0, float(x3_manual_mm))
        else:
            x_target = max(1.0, float(x3_manual_mm))

    ax.set_xlim(0, x_target)
    ax.grid(True, alpha=0.5)
    ax.tick_params(axis='both', labelsize=8)
    ax.set_facecolor('white')
    if mode == "turn":
        ax.set_xlabel('Voltas (pino)', fontsize=9)
        ax.set_title('Processamento por volta', fontsize=10)
    else:
        ax.set_xlabel('Distancia [mm]', fontsize=9)
        ax.set_title('Processamento por distancia', fontsize=10)
    ax.set_ylabel('Atrito [-]', fontsize=9)
    if not y3_auto:
        ax.set_ylim(y3_min, y3_max)
    if twin_ax is not None:
        twin_ax.set_ylabel('Velocidade [mm/s]', fontsize=8, color='darkorange')
        twin_ax.yaxis.set_label_coords(1.06, 0.5)
        twin_ax.tick_params(axis='y', labelsize=7, colors='darkorange')
        twin_ax.grid(False)
        if len(vel_plot) > 0 and np.any(np.isfinite(vel_plot)):
            vmax = np.nanmax(vel_plot)
            if np.isfinite(vmax):
                twin_ax.set_ylim(0, max(1.0, float(vmax) * 1.15))
    h1, l1 = ax.get_legend_handles_labels()
    h2, l2 = twin_ax.get_legend_handles_labels() if twin_ax is not None else ([], [])
    handles = h1 + h2
    labels = l1 + l2
    if handles:
        ax.legend(handles, labels, loc='upper right', fontsize='x-small')


def update_graficos_tab(frame):
    """
    Atualiza a aba de visualizacao com 4 graficos simultaneos:
    - Temperatura e CoF (esquerda)
    - Atrito por volta e por distancia (direita)
    """
    try:
        _plot_temp_axis_view(axg_temp)
        _plot_cof_axis_view(axg_cof)
        _plot_g3_mode_axis_view(axg_turn, "turn", axg_turn_twin)
        _plot_g3_mode_axis_view(axg_dist, "dist", axg_dist_twin)
    except Exception as e:
        log_msg(f"Aba graficos: erro de atualizacao ({e}).")


def _save_graficos_tab_image(path=None):
    """
    Salva uma imagem PNG da figura 2x2 da aba "graficos".
    Usa a propria figura Matplotlib para nao depender do foco da janela.
    """
    global graph_snapshot_rendering

    target = path or globals().get("caminho_graficos_png", "")
    if not target:
        base_dir = os.path.dirname(caminho_merge_csv) if caminho_merge_csv else caminho_pasta
        if not base_dir:
            return False
        target = os.path.join(base_dir, "graficos.png")

    try:
        os.makedirs(os.path.dirname(target), exist_ok=True)
    except Exception:
        pass

    previous_snapshot_state = graph_snapshot_rendering
    graph_snapshot_rendering = True
    try:
        update_graficos_tab(None)
        try:
            canvas_graficos.draw()
        except Exception:
            pass
        fig_graficos.savefig(target, dpi=150)
        log_msg(f"Imagem dos graficos salva: {target}")
        graph_log(f"GRAPH image saved: {target}")
        return True
    except Exception as e:
        log_msg(f"Aviso: nao foi possivel salvar imagem dos graficos ({e}).")
        graph_log(f"GRAPH image save failed: {e}")
        return False
    finally:
        graph_snapshot_rendering = previous_snapshot_state



# get_ip():
# - Stub da versao anterior; funcionalidade historica de leitura manual de IP.
# - Mantido para evitar NameError em referencias antigas.
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
# get_nome():
# - Stub da versao anterior para compatibilidade com versoes antigas da UI.
def get_nome():
    global nome  # Declare ip as a global variable
    nome = receive_nome_entry.get()  # Update the global variable with the entry value
    print(f"nome received: {nome}")  # Print the IP value after it's been updated
"""


# get_aux():
# - Le valor do spinbox de pontos e atualiza aux.
# - aux controla quantos pontos ficam visiveis na janela dos graficos.
def get_aux():
    global aux
    try:
        aux = int(aux_spinbox.get())
    except:
        aux_label = tkinter.Label(labelframe1_lb1_3, text=f"configurado:{aux} ", fg="black", bg="white")
        aux_label.grid(row=3, column=3, sticky="nw")


def _calc_total_duration_min_from_table():
    """
    Soma as duracoes preenchidas na tabela inferior (em minutos).

    Retorno:
        float >= 0.0
    """
    total_min = 0.0
    for lbl in lista_labels_duracao:
        try:
            txt = lbl.cget("text").strip().replace(",", ".")
        except Exception:
            continue
        if not txt or txt in ("xxxx", "Erro"):
            continue
        try:
            v = float(txt)
        except Exception:
            continue
        if v > 0:
            total_min += v
    return total_min


def _current_x_window_min():
    """
    Retorna a janela de eixo X usada pelos graficos 1 e 2.
    """
    if monitor_mode_active:
        return MONITOR_WINDOW_MIN
    if x_auto:
        # Durante ensaio, prioriza a duracao prevista congelada no START.
        if _is_running() and x_auto_total_min > 0:
            return x_auto_total_min
        # Fora de ensaio, usa a soma atual da tabela.
        total_tbl = _calc_total_duration_min_from_table()
        if total_tbl > 0:
            return total_tbl
        if x_auto_total_min > 0:
            return x_auto_total_min
    return max(0.01, float(x_manual_min))


def _x_plot_series(t_data, y_data):
    """
    Prepara serie temporal para os graficos 1/2 conforme modo do eixo X.

    Modo automatico:
        tempo relativo continuo desde o inicio do ensaio.
    Modo manual:
        janela ciclica em minutos; ao completar a janela, reinicia em 0.
    """
    global x_time_ref_s

    if len(t_data) == 0:
        return np.array([]), np.array([])

    if x_time_ref_s is None:
        x_time_ref_s = float(t_data[0])

    t_rel_min = (t_data - x_time_ref_s) / 60.0

    if monitor_mode_active:
        t_end = float(t_rel_min[-1]) if len(t_rel_min) > 0 else 0.0
        t_ini = max(0.0, t_end - MONITOR_WINDOW_MIN)
        mask = (t_rel_min >= t_ini)
        t_plot = t_rel_min[mask] - t_ini
        y_plot = y_data[mask]
        return t_plot, y_plot

    if x_auto:
        return t_rel_min, y_data

    janela_min = max(0.01, float(x_manual_min))
    # Identifica o ciclo atual da janela manual e exibe apenas esse bloco.
    ciclos = np.floor(np.maximum(t_rel_min, 0.0) / janela_min)
    ciclo_atual = np.max(ciclos) if len(ciclos) > 0 else 0.0
    mask = (ciclos == ciclo_atual)
    t_plot = t_rel_min[mask] - (ciclo_atual * janela_min)
    y_plot = y_data[mask]
    return t_plot, y_plot


def _downsample_for_plot(x_arr, *y_arrs, max_points=1200):
    """
    Reduz quantidade de pontos apenas para exibicao em tela.

    Mantem os dados completos em memoria/arquivo e aplica thinning uniforme
    apenas no caminho de renderizacao para reduzir custo de redraw.
    """
    try:
        n = len(x_arr)
    except Exception:
        return (x_arr,) + y_arrs
    if n <= 0 or n <= int(max_points):
        return (x_arr,) + y_arrs
    step = max(1, int(math.ceil(float(n) / float(max_points))))
    sl = slice(None, None, step)
    out = [x_arr[sl]]
    for y in y_arrs:
        try:
            out.append(y[sl] if len(y) == n else y)
        except Exception:
            out.append(y)
    return tuple(out)


def _set_graph3_mode(mode):
    """
    Atualiza modo de visualizacao do grafico 3.

    Args:
        mode (str): "dist" para processamento por distancia, "turn" para por volta.
    """
    global graph3_mode
    mode = str(mode).strip().lower()
    if mode not in ("dist", "turn"):
        mode = "dist"
    graph3_mode = mode
    if "graph3_mode_var" in globals():
        graph3_mode_var.set(mode)
    _bump_graph_config_revision()
    try:
        _request_graph_render(force=True)
    except Exception:
        pass


def abrir_config_x():
    """
    Abre popup compacto para configurar eixo X dos graficos 1/2 e 3.

    Regras:
    - Configuracao compartilhada entre Temperatura e CoF.
    - Automatico: usa duracao total prevista do ensaio.
    - Manual: usa janela fixa em minutos e reinicia o grafico a cada ciclo.
    - Grafico 3: configuracao separada por distancia (mm).
    """
    global x_auto, x_manual_min, x_auto_total_min, x_time_ref_s
    global x3_auto, x3_manual_mm, x3_auto_total_mm, aux

    win = tkinter.Toplevel(root)
    win.title("Configurar Eixo X")
    win.resizable(False, False)
    win.transient(root)
    win.grab_set()

    frame = tkinter.Frame(win, padx=8, pady=8)
    frame.grid(row=0, column=0, sticky="nsew")

    tkinter.Label(frame, text="Eixo X (Temperatura + CoF)", font=("Arial", 9, "bold")) \
        .grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, 6))
    tkinter.Label(frame, text="Automatico", font=("Arial", 9, "bold")).grid(row=1, column=0, padx=4, pady=(0, 4))
    tkinter.Label(frame, text="Manual [min]", font=("Arial", 9, "bold")).grid(row=1, column=1, padx=4, pady=(0, 4))

    x_auto_var = tkinter.BooleanVar(value=x_auto)
    x_manual_var = tkinter.StringVar(value=f"{x_manual_min:.6g}")

    chk_auto = tkinter.Checkbutton(frame, variable=x_auto_var)
    chk_auto.grid(row=2, column=0, padx=4, pady=2)
    ent_manual = tkinter.Entry(frame, width=12, textvariable=x_manual_var)
    ent_manual.grid(row=2, column=1, padx=4, pady=2)

    tkinter.Label(frame, text="Eixo X (Grafico 3 - Distancia)", font=("Arial", 9, "bold")) \
        .grid(row=3, column=0, columnspan=3, sticky="w", pady=(10, 6))
    tkinter.Label(frame, text="Automatico", font=("Arial", 9, "bold")).grid(row=4, column=0, padx=4, pady=(0, 4))
    tkinter.Label(frame, text="Manual [mm]", font=("Arial", 9, "bold")).grid(row=4, column=1, padx=4, pady=(0, 4))

    x3_auto_var = tkinter.BooleanVar(value=x3_auto)
    x3_manual_var = tkinter.StringVar(value=f"{x3_manual_mm:.6g}")

    chk3_auto = tkinter.Checkbutton(frame, variable=x3_auto_var)
    chk3_auto.grid(row=5, column=0, padx=4, pady=2)
    ent3_manual = tkinter.Entry(frame, width=12, textvariable=x3_manual_var)
    ent3_manual.grid(row=5, column=1, padx=4, pady=2)

    lbl_info_12 = tkinter.Label(frame, text="", fg="gray25")
    lbl_info_12.grid(row=6, column=0, columnspan=3, sticky="w", pady=(4, 0))
    lbl_info_3 = tkinter.Label(frame, text="", fg="gray25")
    lbl_info_3.grid(row=7, column=0, columnspan=3, sticky="w", pady=(2, 0))

    def _refresh_state(*_args):
        ent_manual.config(state=("disabled" if x_auto_var.get() else "normal"))
        ent3_manual.config(state=("disabled" if x3_auto_var.get() else "normal"))

        if x_auto_var.get():
            previsto = _calc_total_duration_min_from_table()
            if previsto > 0:
                lbl_info_12.config(text=f"Graficos 1/2 automatico: duracao prevista = {previsto:.2f} min")
            else:
                lbl_info_12.config(text="Graficos 1/2 automatico: duracao sera congelada ao iniciar ensaio.")
        else:
            lbl_info_12.config(text="Graficos 1/2 manual: ao completar a janela, reinicia em 0.")

        if x3_auto_var.get():
            previsto_mm = _calc_expected_distance_mm_from_table()
            if previsto_mm > 0:
                lbl_info_3.config(text=f"Grafico 3 automatico: distancia prevista = {previsto_mm:.2f} mm")
            else:
                lbl_info_3.config(text="Grafico 3 automatico: distancia prevista sera congelada no START.")
        else:
            lbl_info_3.config(text="Grafico 3 manual: janela em mm com varredura ciclica.")

    x_auto_var.trace_add("write", _refresh_state)
    x3_auto_var.trace_add("write", _refresh_state)
    _refresh_state()

    btns = tkinter.Frame(frame)
    btns.grid(row=8, column=0, columnspan=3, sticky="e", pady=(8, 0))

    def _apply():
        global x_auto, x_manual_min, x_auto_total_min
        global x3_auto, x3_manual_mm, x3_auto_total_mm, aux

        if x_auto_var.get():
            x_auto = True
            previsto = _calc_total_duration_min_from_table()
            if previsto > 0:
                x_auto_total_min = previsto
            log_msg(f"Eixo X em automatico (duracao prevista={x_auto_total_min:.2f} min).")
        else:
            try:
                v = float(x_manual_var.get().strip().replace(",", "."))
                if v <= 0:
                    raise ValueError()
            except Exception:
                messagebox.showwarning(
                    "Valor Invalido",
                    "Tempo manual do eixo X deve ser um numero positivo (minutos)."
                )
                return
            x_auto = False
            x_manual_min = v
            # Mantem buffer suficiente para uma janela manual completa.
            aux = max(1, int(math.ceil(x_manual_min * 60.0 * freq)))
            log_msg(f"Eixo X manual: janela={x_manual_min:.2f} min (aux={aux}).")

        if x3_auto_var.get():
            x3_auto = True
            previsto_mm = _calc_expected_distance_mm_from_table()
            if previsto_mm > 0:
                x3_auto_total_mm = previsto_mm
            log_msg(f"Eixo X G3 em automatico (distancia prevista={x3_auto_total_mm:.2f} mm).")
        else:
            try:
                v3 = float(x3_manual_var.get().strip().replace(",", "."))
                if v3 <= 0:
                    raise ValueError()
            except Exception:
                messagebox.showwarning(
                    "Valor Invalido",
                    "Distancia manual do eixo X (Grafico 3) deve ser um numero positivo em mm."
                )
                return
            x3_auto = False
            x3_manual_mm = v3
            log_msg(f"Eixo X G3 manual: janela={x3_manual_mm:.2f} mm.")

        _bump_graph_config_revision()
        try:
            _request_graph_render(force=True)
        except Exception:
            pass
        win.destroy()

    tkinter.Button(btns, text="Aplicar", width=10, command=_apply).pack(side="left", padx=(0, 6))
    tkinter.Button(btns, text="Cancelar", width=10, command=win.destroy).pack(side="left")

# abrir_config_y():
# - Abre popup compacto para configurar eixo Y dos graficos 1, 2 e 3.
# - Cada grafico pode ficar em automatico ou manual (min/max).
# - Aplica validacao numerica e redesenha canvas ao confirmar.
def abrir_config_y():
    """Abre popup compacto para configurar eixo Y dos graficos 1, 2 e 3."""
    global y1_auto, y1_min, y1_max, y2_auto, y2_min, y2_max, y3_auto, y3_min, y3_max

    win = tkinter.Toplevel(root)
    win.title("Configurar Eixos Y")
    win.resizable(False, False)
    win.transient(root)
    win.grab_set()

    frame = tkinter.Frame(win, padx=8, pady=8)
    frame.grid(row=0, column=0, sticky="nsew")

    tkinter.Label(frame, text="Grafico", font=("Arial", 9, "bold")).grid(row=0, column=0, padx=(0, 8), pady=(0, 4), sticky="w")
    tkinter.Label(frame, text="Automatico", font=("Arial", 9, "bold")).grid(row=0, column=1, padx=4, pady=(0, 4))
    tkinter.Label(frame, text="Min", font=("Arial", 9, "bold")).grid(row=0, column=2, padx=4, pady=(0, 4))
    tkinter.Label(frame, text="Max", font=("Arial", 9, "bold")).grid(row=0, column=3, padx=4, pady=(0, 4))

    y_temp_auto_var = tkinter.BooleanVar(value=y1_auto)
    y_temp_min_var = tkinter.StringVar(value=f"{y1_min:.6g}")
    y_temp_max_var = tkinter.StringVar(value=f"{y1_max:.6g}")

    y_forca_auto_var = tkinter.BooleanVar(value=y2_auto)
    y_forca_min_var = tkinter.StringVar(value=f"{y2_min:.6g}")
    y_forca_max_var = tkinter.StringVar(value=f"{y2_max:.6g}")

    y_atrito_auto_var = tkinter.BooleanVar(value=y3_auto)
    y_atrito_min_var = tkinter.StringVar(value=f"{y3_min:.6g}")
    y_atrito_max_var = tkinter.StringVar(value=f"{y3_max:.6g}")

    tkinter.Label(frame, text="Temperatura (Grafico 1)").grid(row=1, column=0, padx=(0, 8), pady=2, sticky="w")
    chk_temp = tkinter.Checkbutton(frame, variable=y_temp_auto_var)
    chk_temp.grid(row=1, column=1, padx=4, pady=2)
    ent_temp_min = tkinter.Entry(frame, width=9, textvariable=y_temp_min_var)
    ent_temp_min.grid(row=1, column=2, padx=4, pady=2)
    ent_temp_max = tkinter.Entry(frame, width=9, textvariable=y_temp_max_var)
    ent_temp_max.grid(row=1, column=3, padx=4, pady=2)

    tkinter.Label(frame, text="CoF (Grafico 2)").grid(row=2, column=0, padx=(0, 8), pady=2, sticky="w")
    chk_forca = tkinter.Checkbutton(frame, variable=y_forca_auto_var)
    chk_forca.grid(row=2, column=1, padx=4, pady=2)
    ent_forca_min = tkinter.Entry(frame, width=9, textvariable=y_forca_min_var)
    ent_forca_min.grid(row=2, column=2, padx=4, pady=2)
    ent_forca_max = tkinter.Entry(frame, width=9, textvariable=y_forca_max_var)
    ent_forca_max.grid(row=2, column=3, padx=4, pady=2)

    tkinter.Label(frame, text="Atrito por distancia (Grafico 3)").grid(row=3, column=0, padx=(0, 8), pady=2, sticky="w")
    chk_atrito = tkinter.Checkbutton(frame, variable=y_atrito_auto_var)
    chk_atrito.grid(row=3, column=1, padx=4, pady=2)
    ent_atrito_min = tkinter.Entry(frame, width=9, textvariable=y_atrito_min_var)
    ent_atrito_min.grid(row=3, column=2, padx=4, pady=2)
    ent_atrito_max = tkinter.Entry(frame, width=9, textvariable=y_atrito_max_var)
    ent_atrito_max.grid(row=3, column=3, padx=4, pady=2)

    def _set_manual_state(var, ent_min, ent_max):
        state = "disabled" if var.get() else "normal"
        ent_min.config(state=state)
        ent_max.config(state=state)

    def _refresh_states(*_args):
        _set_manual_state(y_temp_auto_var, ent_temp_min, ent_temp_max)
        _set_manual_state(y_forca_auto_var, ent_forca_min, ent_forca_max)
        _set_manual_state(y_atrito_auto_var, ent_atrito_min, ent_atrito_max)

    y_temp_auto_var.trace_add("write", _refresh_states)
    y_forca_auto_var.trace_add("write", _refresh_states)
    y_atrito_auto_var.trace_add("write", _refresh_states)
    _refresh_states()

    btns = tkinter.Frame(frame)
    btns.grid(row=4, column=0, columnspan=4, sticky="e", pady=(8, 0))

    def _parse_range(nome, is_auto, txt_min, txt_max):
        if is_auto:
            return None
        try:
            vmin = float(txt_min.replace(",", ".").strip())
            vmax = float(txt_max.replace(",", ".").strip())
        except Exception:
            messagebox.showwarning("Valor invalido", f"{nome}: min/max devem ser numericos.", parent=win)
            return "ERR"
        if vmin >= vmax:
            messagebox.showwarning("Valor invalido", f"{nome}: min deve ser menor que max.", parent=win)
            return "ERR"
        return (vmin, vmax)

    def _aplicar():
        global y1_auto, y1_min, y1_max, y2_auto, y2_min, y2_max, y3_auto, y3_min, y3_max
        r_temp = _parse_range("Temperatura", y_temp_auto_var.get(), y_temp_min_var.get(), y_temp_max_var.get())
        if r_temp == "ERR":
            return
        r_forca = _parse_range("CoF", y_forca_auto_var.get(), y_forca_min_var.get(), y_forca_max_var.get())
        if r_forca == "ERR":
            return
        r_atrito = _parse_range("Atrito por distancia", y_atrito_auto_var.get(), y_atrito_min_var.get(), y_atrito_max_var.get())
        if r_atrito == "ERR":
            return

        y1_auto = y_temp_auto_var.get()
        y2_auto = y_forca_auto_var.get()
        y3_auto = y_atrito_auto_var.get()

        if r_temp is not None:
            y1_min, y1_max = r_temp
        if r_forca is not None:
            y2_min, y2_max = r_forca
        if r_atrito is not None:
            y3_min, y3_max = r_atrito

        _bump_graph_config_revision()
        try:
            _request_graph_render(force=True)
        except Exception:
            pass
        win.destroy()

    tkinter.Button(btns, text="Aplicar alteracoes", command=_aplicar).grid(row=0, column=0, padx=(0, 6))
    tkinter.Button(btns, text="Cancelar", command=win.destroy).grid(row=0, column=1)
# aumenta_tensao():
# - Stub da versao anterior de ajuste manual de tensao analogica (+0.02 V).
# - Desativado no fluxo atual para evitar comandos indevidos.
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

# diminui_tensao():
# - Stub da versao anterior de ajuste manual de tensao analogica (-0.02 V).
# - Desativado no fluxo atual para evitar comandos indevidos.
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
    """
    Stub da versao anterior de configuracao de canais.

    Contexto:
        Em versoes antigas havia fluxo manual de selecao de canais.
        No fluxo atual, canais do DLG sao tratados pelo logger externo e
        pela calibracao dedicada (CalibraDLG/CalibraDLG_UI).
    """
    _bump_graph_config_revision()
    try:
        _request_graph_render(force=True)
    except Exception:
        pass

# variáveis globais
# Variaveis globais historicas da UI (mantidas por compatibilidade).
# Nota: parte dessas variaveis duplica estado definido acima; como este arquivo
# evoluiu em camadas, ainda existem referencias legadas espalhadas no codigo.
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

# Buffers legados de cronograma por tensao/tempo.
tensao_vel = []
qte_vel = 0
duracao = []
soma_tempos_vel = []
soma_duracoes = 0









#############   tkinter

# cria interface gráfica
root = tkinter.Tk()
root.title("Software reciprocating LATRIB")
_apply_app_icon(root)
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

nb.add(aba2, text="configuracoes iniciais")

# ------------------------------------------------------------------
# ABA DE CONFIGURACOES ADICIONAIS
# ------------------------------------------------------------------
aba_cfg = tkinter.Frame(nb)
aba_cfg.grid(sticky="news", row=0, column=0)
aba_cfg.grid_rowconfigure(0, weight=1)
aba_cfg.grid_columnconfigure(0, weight=1)
nb.add(aba_cfg, text="configuracoes adicionais")

# ------------------------------------------------------------------
# ABA DE GRAFICOS (VISUALIZACAO 2x2)
# ------------------------------------------------------------------
aba_graficos = tkinter.Frame(nb)
aba_graficos.grid(sticky="news", row=0, column=0)
aba_graficos.grid_rowconfigure(0, weight=1)
aba_graficos.grid_columnconfigure(0, weight=1)
nb.add(aba_graficos, text="graficos")

cfg_frame = tkinter.LabelFrame(aba_cfg, text="configuracoes")
cfg_frame.grid(row=0, column=0, sticky="nsew", padx=8, pady=8)
cfg_frame.grid_columnconfigure(1, weight=1)

tkinter.Label(
    cfg_frame,
    text="Diretorio fixo dos ensaios",
    font=("Arial", 10, "bold")
).grid(row=0, column=0, sticky="w", padx=6, pady=(8, 4), columnspan=3)

repo_base_var = tkinter.StringVar(value=REPO_BASE)
repo_entry = tkinter.Entry(cfg_frame, textvariable=repo_base_var, state="readonly")
repo_entry.grid(row=1, column=0, columnspan=2, sticky="ew", padx=6, pady=4)

btn_repo = tkinter.Button(cfg_frame, text="Selecionar pasta", command=selecionar_repo_base)
btn_repo.grid(row=1, column=2, sticky="e", padx=6, pady=4)

tkinter.Label(
    cfg_frame,
    text="A pasta selecionada fica salva e sera usada como base nos proximos ensaios.",
    anchor="w",
    justify="left"
).grid(row=2, column=0, columnspan=3, sticky="w", padx=6, pady=(2, 12))

tkinter.Label(
    cfg_frame,
    text="Relacao mecanica (i = D2/D1)",
    font=("Arial", 10, "bold")
).grid(row=3, column=0, sticky="w", padx=6, pady=(8, 4), columnspan=3)

relacao_var = tkinter.StringVar(value=f"{RELACAO_MECANICA:.6g}")
entry_relacao = tkinter.Entry(cfg_frame, textvariable=relacao_var)
entry_relacao.grid(row=4, column=0, sticky="ew", padx=6, pady=(2, 4))

btn_salvar_rel = tkinter.Button(cfg_frame, text="Salvar relacao", command=salvar_relacao_mecanica)
btn_salvar_rel.grid(row=4, column=1, sticky="w", padx=6, pady=(2, 4))

tkinter.Label(
    cfg_frame,
    text="Formula RPM: (i * v * 60) / (2 * pi * raio), com i = D2/D1.",
    anchor="w",
    justify="left"
).grid(row=5, column=0, columnspan=3, sticky="w", padx=6, pady=(2, 10))

tkinter.Label(
    cfg_frame,
    text="Tamanho do intervalo (mm)",
    font=("Arial", 10, "bold")
).grid(row=6, column=0, sticky="w", padx=6, pady=(8, 4), columnspan=3)

dist_interval_var = tkinter.StringVar(value=f"{DIST_INTERVAL_MM:.6g}")
entry_dist_interval = tkinter.Entry(cfg_frame, textvariable=dist_interval_var)
entry_dist_interval.grid(row=7, column=0, sticky="ew", padx=6, pady=(2, 4))

btn_salvar_intervalo = tkinter.Button(cfg_frame, text="Salvar intervalo", command=salvar_intervalo_distancia)
btn_salvar_intervalo.grid(row=7, column=1, sticky="w", padx=6, pady=(2, 4))

tkinter.Label(
    cfg_frame,
    text="Grafico 3 e arquivo _P agregam por blocos de distancia deste tamanho.",
    anchor="w",
    justify="left"
).grid(row=8, column=0, columnspan=3, sticky="w", padx=6, pady=(2, 10))

tkinter.Label(
    cfg_frame,
    text="Calibracao de canais",
    font=("Arial", 10, "bold")
).grid(row=9, column=0, sticky="w", padx=6, pady=(8, 4), columnspan=3)

btn_cfg_canais = tkinter.Button(cfg_frame, text="Configurar canais", command=abrir_configurar_canais)
btn_cfg_canais.grid(row=10, column=0, sticky="w", padx=6, pady=(2, 6))

tkinter.Label(
    cfg_frame,
    text="Recomendado: sem ensaio ativo e sem processos de aquisicao em segundo plano.",
    anchor="w",
    justify="left"
).grid(row=11, column=0, columnspan=3, sticky="w", padx=6, pady=(2, 8))

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

# muda_estado_lub():
# - Habilita/desabilita campos de material/% quando "Lubrificado" muda.
# - Tambem aciona recalculo derivado das tabelas quando necessario.
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

# muda_estado_reciprocante():
# - Habilita/desabilita campo de curso conforme checkbox "Reciprocating".
# - Dispara recalc de voltas/cursos/duracao para manter UI consistente.
def muda_estado_reciprocante():
    if reciprocante_var.get():
        chk_reciprocante.config(selectcolor="blue")
        entry_curso.config(state='normal')

        if 'lbl_header_voltas_cursos' in globals():
            lbl_header_voltas_cursos.config(text="Cursos_mot")
        if 'lbl_header_voltas_pin' in globals():
            lbl_header_voltas_pin.config(text="Cursos_pin")
    else:
        chk_reciprocante.config(selectcolor="white")
        entry_curso.config(state='disabled')
        
        if 'lbl_header_voltas_cursos' in globals():
            lbl_header_voltas_cursos.config(text='Voltas_mot')
        if 'lbl_header_voltas_pin' in globals():
            lbl_header_voltas_pin.config(text='Voltas_pin')

    if 'calcular_voltas_cursos_duracao' in globals():
        calcular_voltas_cursos_duracao()


# calcular_voltas_cursos_duracao(event=None):
# - Recalcula colunas derivadas da tabela de etapas:
#   voltas, numero de cursos e duracao estimada.
# - Entrada vem de velocidade/distancia/curso informados pelo usuario.
# - Atualiza labels linha a linha na tabela inferior.
def calcular_voltas_cursos_duracao(event=None):
    """
    Atualiza as colunas Voltas motor/Voltas pino (ou Cursos) e Duracao com base em:
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
            lista_labels_voltas_pin[i].config(text="xxxx")
            lista_labels_velocidade_real[i].config(text="xxxx")
            lista_labels_duracao[i].config(text="xxxx")
            continue

        try:
            vel = float(vel_txt) if vel_txt else None
            dist = float(dist_txt) if dist_txt else None
        except Exception:
            lista_labels_voltas_cursos[i].config(text="Erro")
            lista_labels_voltas_pin[i].config(text="Erro")
            lista_labels_velocidade_real[i].config(text="Erro")
            lista_labels_duracao[i].config(text="Erro")
            continue

        if vel is not None and vel > 0:
            _, vel_real = _preview_real_pin_speed_mm_s(vel, raio)
            if vel_real is not None:
                lista_labels_velocidade_real[i].config(text=f"{vel_real:.3f}")
            else:
                lista_labels_velocidade_real[i].config(text="Erro")
        else:
            lista_labels_velocidade_real[i].config(text="Erro")

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
                lista_labels_voltas_pin[i].config(text="Erro")
            else:
                cursos = dist / curso
                lista_labels_voltas_cursos[i].config(text=f"{cursos:.2f}")
                # Em modo reciprocante mantemos o mesmo valor nas duas colunas.
                lista_labels_voltas_pin[i].config(text=f"{cursos:.2f}")
        else:
            if raio is None or raio <= 0 or dist is None:
                lista_labels_voltas_cursos[i].config(text="Erro")
                lista_labels_voltas_pin[i].config(text="Erro")
            else:
                # Voltas fisicas no pino dependem apenas de distancia e raio.
                voltas_pin = dist / (2.0 * 3.141592653589793 * raio)
                # i = D2 / D1 -> voltas_motor = i * voltas_pino
                if RELACAO_MECANICA > 0:
                    voltas_mot = voltas_pin * RELACAO_MECANICA
                    lista_labels_voltas_cursos[i].config(text=f"{voltas_mot:.2f}")
                    lista_labels_voltas_pin[i].config(text=f"{voltas_pin:.2f}")
                else:
                    lista_labels_voltas_cursos[i].config(text="Erro")
                    lista_labels_voltas_pin[i].config(text="Erro")
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
labelframe2_baixo.grid_columnconfigure(4, weight=2)
labelframe2_baixo.grid_columnconfigure(5, weight=4)

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
labelframe3_baixo.grid_rowconfigure(9, weight=0)
labelframe3_baixo.grid_rowconfigure(10, weight=0)
labelframe3_baixo.grid_columnconfigure(0, weight=1)
labelframe3_baixo.grid_columnconfigure(1, weight=1)


#Cabeçalhos da parte de baixo
tkinter.Label(labelframe2_baixo, text="Velocidade [mm/s]", font=("Arial", 9, "bold")) \
    .grid(row=0, column=0, padx=5, pady=5)
tkinter.Label(labelframe2_baixo, text="Distâncias [mm]", font=("Arial", 9, "bold")) \
    .grid(row=0, column=1, padx=5, pady=5)

global lbl_header_voltas_cursos
lbl_header_voltas_cursos = tkinter.Label(labelframe2_baixo, text="Voltas_mot", font=("Arial", 9, "bold"))
lbl_header_voltas_cursos.grid(row=0, column=2, padx=5, pady=5)

global lbl_header_voltas_pin
lbl_header_voltas_pin = tkinter.Label(labelframe2_baixo, text="Voltas_pin", font=("Arial", 9, "bold"))
lbl_header_voltas_pin.grid(row=0, column=3, padx=5, pady=5)

tkinter.Label(labelframe2_baixo, text="Duração [min]", font=("Arial", 9, "bold")) \
    .grid(row=0, column=5, padx=5, pady=5)
tkinter.Label(labelframe2_baixo, text="Velocidade real [mm/s]", font=("Arial", 9, "bold")) \
    .grid(row=0, column=4, padx=5, pady=5)
tkinter.Label(labelframe3_baixo, text="Estado", font=("Arial", 10, "bold")).grid(row=0, column=0, padx=5, pady=5)
label_ensaio_estado = tkinter.Label(labelframe3_baixo, text="Aguardando novo ensaio", font=("Arial", 10, "bold"), fg="black")
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

    voltas_pin_lbl = tkinter.Label(labelframe2_baixo, text="xxxx")
    voltas_pin_lbl.grid(row=1 + i, column=3, sticky="news", padx=5)
    lista_labels_voltas_pin.append(voltas_pin_lbl)

    vel_real_lbl = tkinter.Label(labelframe2_baixo, text="xxxx")
    vel_real_lbl.grid(row=1 + i, column=4, sticky="news", padx=5)
    lista_labels_velocidade_real.append(vel_real_lbl)

    duracao_lbl = tkinter.Label(labelframe2_baixo, text="xxxx")
    duracao_lbl.grid(row=1 + i, column=5, sticky="news", padx=5)
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
label_ensaio_vel = tkinter.Label(labelframe3_baixo, text="0 mm/s", font=("Arial", 12))
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
button_frame4_iniciar.grid(sticky="news", row=4, column=0,columnspan=2, padx=5, pady=2)
button_frame5_pausar = tkinter.Button(labelframe3_baixo, text="Pausar", command=pause_acquisition)
button_frame5_pausar.grid(sticky="news", row=5, column=0,columnspan=2, padx=5, pady=2)
button_frame5_parar = tkinter.Button(labelframe3_baixo, text="Parar", command=stop_acquisition)
button_frame5_parar.grid(sticky="news", row=6, column=0,columnspan=2, padx=5, pady=2)

# Bit de monitoramento (CH1/CH2 sem ensaio), bloqueia inicio enquanto ativo.
monitor_mode_var = tkinter.BooleanVar(value=False)
chk_monitor = tkinter.Checkbutton(
    labelframe3_baixo,
    text="Monitoramento",
    variable=monitor_mode_var,
    command=toggle_monitor_mode
)
chk_monitor.grid(sticky="w", row=7, column=0, columnspan=2, padx=5, pady=(2, 2))
_set_start_button_state()

# Grupo tecnico: tara e check de comunicacao.
button_frame7_zerar = tkinter.Button(labelframe3_baixo, text="Zerar celula", command=zerar_celula)
button_frame7_zerar.grid(sticky="news", row=8, column=0, columnspan=2, padx=5, pady=(14, 2))

# Check status (DLG + Drive) - separado dos botoes principais
button_frame6_status = tkinter.Button(labelframe3_baixo, text="Check status", command=check_status)
button_frame6_status.grid(sticky="news", row=9, column=0, columnspan=2, padx=5, pady=(2, 2))

status_frame = tkinter.Frame(labelframe3_baixo)
status_frame.grid(sticky="w", row=10, column=0, columnspan=2, padx=5, pady=(2, 6))
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
# Criando o gráfico - Tudo abaixo ate o agendador central e criacao/render dos graficos
# --- BARRA DE CONTROLE DE ESCALA (DENTRO DO FRAME DOS GRÁFICOS) ---
frame_toolbar = tkinter.Frame(frame_graficos, bg="#d9d9d9", height=40)
frame_toolbar.pack(side="top", fill="x", padx=5, pady=5)

# --- CONTROLE EIXO X
btn_config_x = tkinter.Button(
    frame_toolbar,
    text="Configurar eixo X",
    command=abrir_config_x,
    font=("Arial", 9)
)
btn_config_x.pack(side="left", padx=5, pady=3)

# Separador visual
tkinter.Label(frame_toolbar, text="|", bg="#d9d9d9", fg="gray").pack(side="left", padx=10)

# --- CONTROLE EIXO Y
btn_config_y = tkinter.Button(frame_toolbar, text="Configurar Eixos Y (Min / Max)", command=abrir_config_y, font=("Arial", 9))
btn_config_y.pack(side="left", padx=5)

# --- MODO DO GRAFICO 3 (distancia x volta)
tkinter.Label(frame_toolbar, text="|", bg="#d9d9d9", fg="gray").pack(side="left", padx=10)
tkinter.Label(frame_toolbar, text="G3:", bg="#d9d9d9", font=("Arial", 9, "bold")).pack(side="left", padx=(0, 4))
graph3_mode_var = tkinter.StringVar(value=graph3_mode)
rb_g3_dist = tkinter.Radiobutton(
    frame_toolbar,
    text="Distancia",
    variable=graph3_mode_var,
    value="dist",
    bg="#d9d9d9",
    command=lambda: _set_graph3_mode("dist"),
    font=("Arial", 9)
)
rb_g3_dist.pack(side="left", padx=(0, 4))
rb_g3_turn = tkinter.Radiobutton(
    frame_toolbar,
    text="Volta",
    variable=graph3_mode_var,
    value="turn",
    bg="#d9d9d9",
    command=lambda: _set_graph3_mode("turn"),
    font=("Arial", 9)
)
rb_g3_turn.pack(side="left")

fig1 = Figure(figsize=(6.5, 1.9), dpi=100)
ax1 = fig1.add_subplot(111)

# AJUSTE DE MARGENS GRAF. 1:
# top=0.92: Sobe o grafico (perto do topo, pois nao tem mais titulo)
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
fig3.subplots_adjust(top=0.90, bottom=0.34, right=0.90)
ax3_twin = None
ax3.set_xlabel('Distancia [mm]', fontsize=9)
ax3.set_ylabel('Atrito [-]', fontsize=9)
ax3.grid(True, alpha=0.5)
ax3.set_facecolor('white')

canvas3 = FigureCanvasTkAgg(fig3, master=frame_graficos)
# side="top" ou "bottom" para empilhar os gráficos um embaixo do outro
canvas3.get_tk_widget().pack(side="top", fill="both", expand=True, padx=5, pady=2)

# Figura da aba "graficos" (4 painéis simultaneos).
fig_graficos = Figure(figsize=(10.0, 6.5), dpi=100)
axg_temp = fig_graficos.add_subplot(221)
axg_cof = fig_graficos.add_subplot(223)
axg_turn = fig_graficos.add_subplot(222)
axg_dist = fig_graficos.add_subplot(224)
axg_turn_twin = axg_turn.twinx()
axg_dist_twin = axg_dist.twinx()
fig_graficos.subplots_adjust(left=0.06, right=0.93, top=0.95, bottom=0.08, hspace=0.35, wspace=0.24)

canvas_graficos = FigureCanvasTkAgg(fig_graficos, master=aba_graficos)
canvas_graficos.get_tk_widget().pack(side="top", fill="both", expand=True, padx=6, pady=6)

#
# Agendador central dos graficos.
#
# Antes havia uma animacao Matplotlib por figura. Na pratica, os 3 graficos laterais
# continuavam redesenhando quando a aba 2x2 estava aberta, e a aba 2x2 tambem
# redesenhava quando estava escondida. O loop abaixo mantem apenas um timer Tk:
# ele atualiza somente a vista ativa e espera a troca de aba assentar antes de
# chamar Matplotlib.
GRAPH_MAIN_INTERVAL_MS = 220
GRAPH_TAB_INTERVAL_MS = 300
GRAPH_IDLE_INTERVAL_MS = 900
GRAPH_SWITCH_QUIET_MS = 180

graficos_focus_mode = False
graficos_side_saved_width = None
graph_render_job = None
graph_render_busy = False
graph_force_render = True
graph_last_main_revision = None
graph_last_tab_revision = None
graph_switch_quiet_until = 0.0
graph_snapshot_rendering = False


def _selected_tab_is_graficos():
    try:
        tab_id = nb.select()
        tab_txt = str(nb.tab(tab_id, "text")).strip().lower()
        return tab_txt == "graficos"
    except Exception:
        return False


def _graph_next_interval_ms(tab_active):
    if _is_running() or monitor_mode_active:
        return GRAPH_TAB_INTERVAL_MS if tab_active else GRAPH_MAIN_INTERVAL_MS
    return GRAPH_IDLE_INTERVAL_MS


def _schedule_graph_render(delay_ms):
    global graph_render_job
    if graph_render_job is not None:
        return
    try:
        graph_render_job = root.after(max(1, int(delay_ms)), _graph_render_loop)
    except Exception:
        graph_render_job = None


def _request_graph_render(force=False, delay_ms=1):
    global graph_force_render, graph_render_job
    if force:
        graph_force_render = True
    if graph_render_job is not None and force:
        try:
            root.after_cancel(graph_render_job)
        except Exception:
            pass
        graph_render_job = None
    _schedule_graph_render(delay_ms)


def _render_main_graphs():
    update1(None)
    update2(None)
    update3(None)
    canvas1.draw_idle()
    canvas2.draw_idle()
    canvas3.draw_idle()


def _render_graficos_tab():
    update_graficos_tab(None)
    canvas_graficos.draw_idle()


def _graph_render_loop():
    global graph_render_job, graph_render_busy, graph_force_render
    global graph_last_main_revision, graph_last_tab_revision

    graph_render_job = None
    tab_active = _selected_tab_is_graficos()
    next_delay = _graph_next_interval_ms(tab_active)

    try:
        if time.monotonic() < graph_switch_quiet_until:
            _schedule_graph_render(60)
            return

        if graph_render_busy:
            _schedule_graph_render(80)
            return

        revision = _current_graph_revision()
        if tab_active:
            needs_render = graph_force_render or (revision != graph_last_tab_revision)
        else:
            needs_render = graph_force_render or (revision != graph_last_main_revision)

        if needs_render:
            graph_render_busy = True
            if tab_active:
                _render_graficos_tab()
                graph_last_tab_revision = revision
            else:
                _render_main_graphs()
                graph_last_main_revision = revision
            graph_force_render = False
    except Exception as e:
        log_msg(f"Graficos: erro no agendador de renderizacao ({e}).")
    finally:
        graph_render_busy = False
        _schedule_graph_render(next_delay)


def _set_graficos_focus_mode(enabled):
    """
    Alterna modo foco da aba de visualizacao 2x2.

    enabled=True:
      - Colapsa o painel lateral.
      - Desenha apenas a figura 2x2.

    enabled=False:
      - Restaura o painel lateral.
      - Desenha apenas a coluna lateral.
    """
    global graficos_focus_mode, graficos_side_saved_width, graph_switch_quiet_until
    enabled = bool(enabled)
    if graficos_focus_mode == enabled:
        _request_graph_render(force=True, delay_ms=GRAPH_SWITCH_QUIET_MS)
        return

    try:
        if enabled:
            if graficos_side_saved_width is None:
                try:
                    w_now = int(frame_graficos.winfo_width())
                except Exception:
                    w_now = 0
                if w_now <= 10:
                    try:
                        w_now = int(frame_graficos.winfo_reqwidth())
                    except Exception:
                        w_now = 680
                graficos_side_saved_width = max(320, w_now)
            try:
                frame_graficos.pack_propagate(False)
            except Exception:
                pass
            frame_graficos.configure(width=1)
        else:
            restore_w = graficos_side_saved_width if graficos_side_saved_width else 680
            frame_graficos.configure(width=int(restore_w))
            try:
                frame_graficos.pack_propagate(False)
            except Exception:
                pass

        graficos_focus_mode = enabled
        graph_switch_quiet_until = time.monotonic() + (GRAPH_SWITCH_QUIET_MS / 1000.0)
        _request_graph_render(force=True, delay_ms=GRAPH_SWITCH_QUIET_MS)
    except Exception as e:
        log_msg(f"Aba graficos: falha ao alternar modo foco ({e}).")


def _on_notebook_tab_changed(event=None):
    """
    Handler de troca de aba para aplicar modo foco na aba 'graficos'.
    """
    try:
        tab_id = nb.select()
        tab_txt = str(nb.tab(tab_id, "text")).strip().lower()
        _set_graficos_focus_mode(tab_txt == "graficos")
    except Exception as e:
        log_msg(f"Aba graficos: falha ao ler aba ativa ({e}).")


nb.bind("<<NotebookTabChanged>>", _on_notebook_tab_changed)
root.after(0, _on_notebook_tab_changed)
_request_graph_render(force=True, delay_ms=250)

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













