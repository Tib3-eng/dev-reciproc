"""Painel e callbacks do perfil senoidal, isolados da UI historica.

O dicionario app referencia globals do novo_tribometro somente na fronteira
de apresentacao. Threads nao acessam widgets: resultados voltam por after.
Controle/IPC e dados ficam em sinewave_integration e no processo externo.
"""
import json
import os
from pathlib import Path
import tempfile
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import orchestrator_runtime as orch
import sinewave_integration as backend
from sinewave_protocol import derived, make_request, positive


class SinewavePanel:
    def __init__(self, app):
        self.app = app
        self.active = False
        self.cancel = threading.Event()
        self.state = None
        saved = app['APP_SETTINGS'].get('sinewave', {})
        self.enabled = tk.BooleanVar(value=False)  # Partida sempre explicita.
        self.simulation = tk.BooleanVar(value=True)
        self.course = tk.StringVar(value=str(saved.get('course_mm', 4)))
        self.frequency = tk.StringVar(value=str(saved.get('frequency_hz', .2)))
        self.cycles = tk.StringVar(value=str(saved.get('cycles', 2)))
        self.executable = tk.StringVar(value=str(saved.get('executable', '')))
        self.checkbox = tk.Checkbutton(app['labelframe1_baixo'], text='Senoidal (teste)',
                                       variable=self.enabled, command=self.toggle)
        self.checkbox.grid(row=2, column=2, sticky='w', padx=5)
        self.frame = ttk.LabelFrame(app['labelframe1_2_baixo'], text='Parametros do teste senoidal')
        for row, (label, variable) in enumerate((('Curso pico a pico [mm]', self.course),
                                                ('Frequencia [Hz]', self.frequency),
                                                ('Ciclos (ida + volta)', self.cycles))):
            ttk.Label(self.frame, text=label).grid(row=row, column=0, sticky='w', padx=8, pady=5)
            entry = ttk.Entry(self.frame, textvariable=variable, width=14)
            entry.grid(row=row, column=1, sticky='w', padx=8, pady=5)
            variable.trace_add('write', lambda *_: self.preview())
        ttk.Checkbutton(self.frame, text='Simular sem hardware (dados sinteticos)',
                        variable=self.simulation, command=self.preview).grid(row=3, column=0, columnspan=2, sticky='w', padx=8)
        ttk.Button(self.frame, text='Selecionar controlador...', command=self.select_exe).grid(row=4, column=0, padx=8, pady=8)
        ttk.Label(self.frame, textvariable=self.executable, wraplength=330).grid(row=4, column=1, sticky='w')
        self.summary = ttk.Label(self.frame, wraplength=440)
        self.summary.grid(row=5, column=0, columnspan=2, sticky='w', padx=8, pady=8)
        ttk.Label(self.frame, text='A curva e mantida no controlador externo.\n'
                  'Hardware depende da validacao do gerador; simular nao abre COM/DLG.',
                  wraplength=440).grid(row=6, column=0, columnspan=2, sticky='w', padx=8, pady=8)
        self.preview()

    def selected(self):
        return bool(self.enabled.get())

    def select_exe(self):
        selected = filedialog.askopenfilename(title='Controlador senoidal', filetypes=[('Executavel', '*.exe')])
        if selected:
            self.executable.set(selected)

    def toggle(self):
        a = self.app
        if self.active or a['_is_running']() or a['run_finalizing'] or a['tara_running'] or a['monitor_mode_active']:
            self.enabled.set(not self.enabled.get())
            messagebox.showinfo('Modo de ensaio', 'Finalize a operacao atual antes de trocar o modo.')
            return
        if self.selected():
            a['reciprocante_var'].set(False)
            a['muda_estado_reciprocante']()
            a['chk_reciprocante'].configure(state='disabled')
            a['entry_curso'].grid_remove()
            a['lbl_curso'].grid_remove()
            a['labelframe2_baixo'].grid_remove()
            self.frame.grid(row=0, column=0, sticky='nsew')
            a['rb_g3_turn'].configure(text='Stroke')
        else:
            self.frame.grid_remove()
            a['labelframe2_baixo'].grid()
            a['entry_curso'].grid()
            a['lbl_curso'].grid()
            a['chk_reciprocante'].configure(state='normal')
            a['muda_estado_reciprocante']()
        self.preview()
        a['_bump_graph_config_revision']()

    def request(self, directory):
        a = self.app
        value = lambda text: str(text).strip().replace(',', '.')
        request = make_request(value(self.course.get()), value(self.frequency.get()),
                               value(self.cycles.get()), value(a['ent_raio'].get()),
                               a['RELACAO_MECANICA'], directory, self.simulation.get())
        request['force_normal_n'] = positive(value(a['ent_forca'].get()), 'Forca normal [N]')
        request['distance_interval_mm'] = a['DIST_INTERVAL_MM']
        return request

    def preview(self):
        try:
            request = self.request(os.getcwd())
            d = derived(request)
            prefix = 'SIMULACAO' if self.simulation.get() else 'HARDWARE: exige gerador validado'
            self.summary.configure(text=f'{prefix}\nPeriodo: {d["period_s"]:.3g} s | '
                                   f'Duracao: {d["duration_s"]:.3g} s\n'
                                   f'Distancia prevista: {d["distance_mm"]:.3g} mm | '
                                   f'Pico previsto: {d["peak_motor_rpm"]:.3g} RPM')
        except Exception:
            self.summary.configure(text='Preencha curso, frequencia, ciclos, raio e forca normal validos.')

    def _lock(self, locked):
        self.checkbox.configure(state='disabled' if locked else 'normal')
        for widget in self.frame.winfo_children():
            if isinstance(widget, (ttk.Entry, ttk.Button, ttk.Checkbutton)):
                widget.configure(state='disabled' if locked else 'normal')

    def start(self):
        """Entrada chamada depois da validacao comum de identificacao/raio/forca.

        A saida antecipada no programa principal evita consumir a tabela de
        velocidade legada ou habilitar seu controlador de inversoes.
        """
        a = self.app
        if self.active:
            return
        try:
            request = self.request(os.getcwd())
            repo = orch.find_repo_root()
            command = backend.controller_command(repo, self.executable.get().strip())
            with tempfile.TemporaryDirectory(prefix='latrib_sine_validate_') as tmp:
                # Inspecao do gerador SEM hardware, antes inclusive da tara.
                backend.validate_controller(command, request, Path(tmp)/'request.json')
        except Exception as exc:
            messagebox.showerror('Controlador senoidal', str(exc))
            return
        calibration = None
        if not request['simulation']:
            try:
                calibration = a['_load_required_encoder_calibration']()
            except Exception as exc:
                messagebox.showerror('Calibracao obrigatoria', str(exc))
                return
        a['salvar_arquivo']()
        if not a['caminho_arquivo_1']:
            return
        request['output']['directory'] = os.path.join(a['caminho_pasta'], 'DadosDev')
        offset = 0.0
        if not request['simulation']:
            exe = orch.check_executables(repo)
            if not a['_run_auto_tara_before_start'](repo, exe):
                return
            # Preservar preparacao existente em sequencia, sem duas COM abertas.
            # Seu metodo deve ser comparado experimentalmente ao perfil senoidal.
            if not messagebox.askokcancel('Correcao dinamica',
                    'Executar a correcao dinamica atual antes do teste senoidal?\n'
                    'O metodo sera identificado no relatorio. Sua aplicabilidade a senoide requer validacao.'):
                return
            try:
                result = orch.run_reciprocating_offset_capture(
                    repo_root=repo, run_folder=a['caminho_pasta'],
                    course_mm=request['curve']['course_mm'], cycles=a['RECIP_OFFSET_CYCLES'],
                    relacao=a['RELACAO_MECANICA'], raio_mm=request['mechanics']['radius_mm'],
                    tolerance_counts=a['RECIP_STOP_TOL_COUNTS'], rate_hz=200, max_loss_pct=5)
                if not result.get('valid'):
                    raise RuntimeError(result.get('reason', 'Correcao dinamica invalida'))
                offset = float(result['offset_n'])
            except Exception as exc:
                messagebox.showerror('Correcao dinamica', str(exc))
                return
        a['APP_SETTINGS']['sinewave'] = {'course_mm': request['curve']['course_mm'],
            'frequency_hz': request['curve']['frequency_hz'], 'cycles': request['curve']['cycles'],
            'executable': self.executable.get()}
        a['_save_app_settings'](a['APP_SETTINGS'])
        self.active = True
        self.cancel.clear()
        self.state = None
        self._lock(True)
        a['_set_run_finalizing'](True)
        a['label_ensaio_estado'].configure(text='Preparando teste senoidal...', fg='#0078D4')
        metadata = {key: widget.get().strip() for key, widget in a['entries_left'].items()}
        metadata['Lubrificado'] = 'sim' if a['lubrificado_var'].get() else 'nao'
        for index, (mat, pct) in enumerate(zip(a['lista_entries_material'], a['lista_entries_porcentagem']), 1):
            metadata[f'Lubrificante {index}'] = mat.get().strip() + ' ' + pct.get().strip()
        paths = {'merge_csv': a['caminho_merge_csv'], 'turn_dist_csv': a['caminho_turn_dist_csv'],
                 'turn_vp_csv': a['caminho_turn_volta_csv']}
        executable = self.executable.get().strip()
        info_path = a['caminho_arquivo_1']

        def worker():
            error = ''
            state = None
            try:
                state = backend.start_run(repo, paths, request, executable, offset, self.cancel)
                self.state = state
                backend.write_info(info_path, state, metadata, calibration)
                a['root'].after(0, lambda: self._started(state))
                if self.cancel.is_set():
                    state.controller_client.send('stop', reason='operator')
                orch.wait_and_merge(state)
                backend.write_info(info_path, state, metadata, final=True)
                error = state.controller_client.failure
            except Exception as exc:
                error = str(exc)
                # Preservar causa de startup/cancelamento mesmo sem dados.
                if not state:
                    with open(info_path, 'w', encoding='utf-8', newline='') as stream:
                        writer = orch.InfoCsvWriter(stream)
                        writer.writerow('campo', 'valor', 'valor2')
                        writer.writerow('Modo', 'Senoidal (teste)')
                        writer.writerow('SIMULACAO SEM HARDWARE', str(request['simulation']))
                        writer.writerow('Falha na preparacao', error)
            finally:
                a['root'].after(0, lambda: self._finished(state, error))
        threading.Thread(target=worker, daemon=True).start()

    def _started(self, state):
        a = self.app
        d = derived(state.controller_request)
        for key in ('sampsTimestamp', 'p_strokes','p_atrito_ef','p_atrito_max','p_atrito_min',
                    'p_coluna_velocidade','p_turn_strokes','p_turn_atrito_ef','p_turn_atrito_max',
                    'p_turn_atrito_min','p_turn_coluna_velocidade','p_turn_velocidade_alvo'):
            a[key].clear()
        for channel in a['graSamps']:
            channel.clear()
        a['running'] = 'true'
        a['external_run_state'] = state
        a['external_run_token'] += 1
        token = a['external_run_token']
        a['recip_dynamic_offset_n'] = state.dynamic_offset_n
        a['cof_force_normal_n'] = state.force_normal_n
        a['p_dist_target_mm'] = d['distance_mm']
        a['p_turn_target'] = 2 * state.controller_request['curve']['cycles']
        a['x_auto_total_min'] = d['duration_s']/60
        a['x3_auto_total_mm'] = d['distance_mm']
        a['x_time_ref_s'] = None
        a['lista_duracao'] = [d['duration_s']]
        a['is_paused'] = False
        a['_set_run_finalizing'](False)
        a['_start_timer_now']()
        a['button_frame5_pausar'].configure(state='disabled')
        a['label_ensaio_estado'].configure(text='SIMULACAO SENOIDAL' if state.controller_request['simulation'] else 'Teste senoidal', fg='#0078D4')
        a['label_ensaio_vel'].configure(text=f'Pico {d["peak_speed_mm_s"]:.3g} mm/s')
        a['label_ensaio_rpm'].configure(text=f'Pico {d["peak_motor_rpm"]:.3g} rpm')
        a['caminho_dlg_csv'] = state.dlg_csv
        a['caminho_drive_csv'] = state.drive_csv
        threading.Thread(target=a['_tail_dlg_csv_for_graphs'], args=(state.dlg_csv, token), daemon=True).start()
        threading.Thread(target=a['_tail_realtime_reciprocating_encoder'],
                         args=(state.dlg_csv,state.encoder_state_csv,a['DIST_INTERVAL_MM'],[],token), daemon=True).start()
        a['_bump_graph_config_revision']()

    def stop(self):
        if not messagebox.askyesno('Parar teste', 'Encerrar o teste senoidal e salvar os dados parciais?'):
            return
        self.cancel.set()
        if self.state:
            self.state.manual_stop_requested = True
            self.state.controller_client.send('stop', reason='operator')
        self.app['label_ensaio_estado'].configure(text='Finalizando teste senoidal...', fg='darkorange')

    def _finished(self, state, error):
        a = self.app
        a['running'] = False
        a['timer_started'] = False
        a['_set_run_finalizing'](True)
        # after permite que as tails observem running=False antes do reload/PNG.
        def save():
            try:
                if state and os.path.isfile(state.merge_csv):
                    title = 'SIMULACAO SENOIDAL - DADOS SINTETICOS' if state.controller_request['simulation'] else 'Teste senoidal'
                    figure = a['fig_graficos']
                    previous_top = figure.subplotpars.top
                    try:
                        figure.subplots_adjust(top=.90)
                        figure.suptitle(title, fontsize=11)
                        a['_save_graficos_tab_image'](a['caminho_graficos_png'])
                    finally:
                        figure.suptitle('')
                        figure.subplots_adjust(top=previous_top)
                a['label_ensaio_estado'].configure(text='Falha no teste' if error else 'Teste finalizado', fg='red' if error else 'green')
                if error:
                    messagebox.showerror('Teste senoidal', error)
            finally:
                a['_set_targets_stopped']()
                a['external_run_state'] = None
                a['button_frame5_pausar'].configure(state='normal')
                self.active = False
                self.state = None
                self._lock(False)
                a['_set_run_finalizing'](False)
        a['root'].after(100, save)
