/*
MainForm.cs
-----------
Tela principal da calibracao DLG (WinForms), integrada ao CalibraDLG.exe via IPC.

Objetivo geral:
- Guiar o usuario no cadastro de pontos de calibracao (referencia x bruto).
- Controlar o processo externo CalibraDLG.exe por JSON lines (stdin/stdout).
- Exibir status, erros, RMSE e salvar resultado em arquivo JSON por canal.

Fluxo principal da tela:
1) Resolve caminhos de runtime (CalibraDLG.exe e dlg_logger_ipc.exe).
2) Inicia calibracao (config) com canal/sensor/ganho/LPF/excitacao.
3) Para cada ponto, envia "point", aguarda retorno e atualiza grade + RMSE.
4) Finaliza/cancela processo e apresenta resultado ao usuario.

Variaveis principais:
- _proc/_stdin/_readTask/_cts: controle de ciclo de vida do processo IPC.
- _refPoints/_rawPoints: base para calculo incremental de erro (RMSE).
- _pendingPoint: sincroniza requisicao de captura com resposta recebida.
- _calibraExePath/_calibOutDir: caminhos efetivos usados no runtime.
- _finishRequested/_cancelRequested: estados de finalizacao em curso.

Resumo de metodos:
- FindRepoRoot/Get*Candidates/FindFirstExisting/ResolveRuntimePaths:
  localizacao automatica de executaveis.
- HandleLine/SendLine/UI: comunicacao IPC e marshaling para thread de UI.
- SetStatus/AppendLog/SetDlgCheckState/UpdateErrorLabel: atualizacao visual.
- FinishCalib/CancelCalib/ShowLegend: acoes principais de controle da sessao.
- Construtor MainForm: cria widgets, tooltips, grade e wiring de eventos.
*/

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Drawing;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace CalibraDLG_UI
{
    public sealed class MainForm : Form
    {
        private readonly ComboBox _channel;
        private readonly ComboBox _sensor;
        private readonly ComboBox _gain;
        private readonly ComboBox _lpf;
        private readonly ComboBox _sensPwr;
        private readonly Label _tcCjcLabel;
        private readonly ComboBox _tcCjcMode;
        private readonly NumericUpDown _points;
        private readonly TextBox _refValue;
        private readonly Button _start;
        private readonly Button _capture;
        private readonly Button _finish;
        private readonly Button _cancel;
        private readonly Button _legend;
        private readonly Button _checkDlg;
        private readonly DataGridView _grid;
        private readonly TextBox _logBox;
        private readonly Label _status;
        private readonly Label _result;
        private readonly Label _error;
        private readonly Label _dlgCheckStatus;
        private readonly ToolTip _tips;
        private LegendForm? _legendForm;
        private readonly List<double> _refPoints = new();
        private readonly List<double> _rawPoints = new();

        private Process? _proc;
        private StreamWriter? _stdin;
        private CancellationTokenSource? _cts;
        private Task? _readTask;
        private int _expectedPoints;
        private int _capturedPoints;
        private TaskCompletionSource<bool>? _pendingPoint;
        private bool _finishRequested;
        private bool _cancelRequested;
        private string _calibraExePath = string.Empty;
        private string _calibOutDir = string.Empty;
        private bool _updatingSensorUi;

        private const int SensorIndexTermoparK = 8;
        private const int GainIndex1000 = 6;
        private const int LpfUiIndexStrong = 2; // iLPF=1 (UI index -> runtime index mapping)
        private const int SensPwrUiIndex1V = 0;

        public MainForm()
        {
            Text = "CalibraDLG - Calibracao";
            Width = 980;
            Height = 680;

            var top = new TableLayoutPanel
            {
                Dock = DockStyle.Top,
                Height = 122,
                ColumnCount = 6,
                RowCount = 3,
                Padding = new Padding(10),
                AutoSize = false
            };
            top.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 130));
            top.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 45));
            top.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 120));
            top.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 35));
            top.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 210));
            top.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 20));
            top.RowStyles.Add(new RowStyle(SizeType.Absolute, 32));
            top.RowStyles.Add(new RowStyle(SizeType.Absolute, 32));
            top.RowStyles.Add(new RowStyle(SizeType.Absolute, 36));

            _channel = new ComboBox { Dock = DockStyle.Fill, DropDownStyle = ComboBoxStyle.DropDownList };
            for (int i = 1; i <= 8; i++) _channel.Items.Add($"CH{i}");
            _channel.SelectedIndex = 0;

            _sensor = new ComboBox { Dock = DockStyle.Fill, DropDownStyle = ComboBoxStyle.DropDownList };
            _sensor.Items.AddRange(new object[]
            {
                "0 - tensao",
                "1 - corrente",
                "2 - ponte completa",
                "3 - ponte 1/4",
                "4 - meia ponte",
                "5 - ICP",
                "6 - termopar E",
                "7 - termopar J",
                "8 - termopar K",
                "9 - termopar T"
            });
            _sensor.SelectedIndex = 4;

            _gain = new ComboBox { Dock = DockStyle.Fill, DropDownStyle = ComboBoxStyle.DropDownList };
            _gain.Items.AddRange(new object[] { "1", "3", "10", "30", "100", "300", "1000", "3000" });
            _gain.SelectedIndex = 5;

            _lpf = new ComboBox { Dock = DockStyle.Fill, DropDownStyle = ComboBoxStyle.DropDownList };
            _lpf.Items.AddRange(new object[]
            {
                "Padrao (0)",
                "0",
                "1",
                "2",
                "3"
            });
            _lpf.SelectedIndex = 0;

            _sensPwr = new ComboBox { Dock = DockStyle.Fill, DropDownStyle = ComboBoxStyle.DropDownList };
            _sensPwr.Items.AddRange(new object[]
            {
                "1.0 V",
                "2.5 V",
                "3.3 V",
                "5.0 V",
                "Usuario"
            });
            _sensPwr.SelectedIndex = 2;

            _tcCjcLabel = new Label
            {
                Text = "Junta fria",
                AutoSize = true,
                TextAlign = ContentAlignment.MiddleLeft
            };
            _tcCjcMode = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 120 };
            _tcCjcMode.Items.AddRange(new object[]
            {
                "Interna",
                "Externa (TEDs)"
            });
            _tcCjcMode.SelectedIndex = 0;

            _points = new NumericUpDown
            {
                Dock = DockStyle.Fill,
                Minimum = 2,
                Maximum = 20,
                Value = 5
            };

            _refValue = new TextBox { Dock = DockStyle.Fill, Margin = new Padding(0, 4, 0, 4) };

            _start = new Button { Text = "Iniciar", AutoSize = true, Anchor = AnchorStyles.Left, Enabled = true, Margin = new Padding(0, 2, 0, 2) };
            _capture = new Button { Text = "Capturar ponto", AutoSize = true, Anchor = AnchorStyles.Left, Enabled = false, Margin = new Padding(0, 2, 0, 2) };
            _finish = new Button { Text = "Finalizar", AutoSize = true, Anchor = AnchorStyles.Left, Enabled = false, Margin = new Padding(0, 2, 0, 2) };
            _cancel = new Button { Text = "Cancelar", AutoSize = true, Anchor = AnchorStyles.Left, Enabled = false, Margin = new Padding(0, 2, 0, 2) };
            _legend = new Button { Text = "Legenda", AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(0, 2, 0, 2) };
            _checkDlg = new Button { Text = "Check DLG", AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(0, 2, 0, 2) };

            _start.Click += async (_, __) => await StartCalibAsync();
            _capture.Click += async (_, __) => await CapturePointAsync();
            _finish.Click += (_, __) => FinishCalib();
            _cancel.Click += (_, __) => CancelCalib();
            _legend.Click += (_, __) => ShowLegend();
            _checkDlg.Click += async (_, __) => await CheckDlgAsync();
            _sensor.SelectedIndexChanged += (_, __) => OnSensorSelectionChanged();

            var rightButtons = new FlowLayoutPanel
            {
                Dock = DockStyle.Fill,
                AutoSize = false,
                FlowDirection = FlowDirection.RightToLeft,
                WrapContents = false
            };
            rightButtons.Controls.Add(_legend);
            rightButtons.Controls.Add(_cancel);

            var startFinishButtons = new FlowLayoutPanel
            {
                Dock = DockStyle.Fill,
                AutoSize = false,
                FlowDirection = FlowDirection.LeftToRight,
                WrapContents = false
            };
            startFinishButtons.Controls.Add(_start);
            startFinishButtons.Controls.Add(_finish);

            var tcPanel = new FlowLayoutPanel
            {
                Dock = DockStyle.Fill,
                AutoSize = true,
                FlowDirection = FlowDirection.LeftToRight,
                WrapContents = false
            };
            tcPanel.Controls.Add(_tcCjcLabel);
            tcPanel.Controls.Add(_tcCjcMode);

            top.Controls.Add(new Label { Text = "Canal", Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft }, 0, 0);
            top.Controls.Add(_channel, 1, 0);
            top.Controls.Add(new Label { Text = "Sensor", Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft }, 2, 0);
            top.Controls.Add(_sensor, 3, 0);
            top.Controls.Add(new Label { Text = "Ganho", Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft }, 4, 0);
            top.Controls.Add(_gain, 5, 0);

            top.Controls.Add(new Label { Text = "Filtro LPF", Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft }, 0, 1);
            top.Controls.Add(_lpf, 1, 1);
            top.Controls.Add(new Label { Text = "Excitacao", Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft }, 2, 1);
            top.Controls.Add(_sensPwr, 3, 1);
            top.Controls.Add(new Label { Text = "Pontos", Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft }, 4, 1);
            top.Controls.Add(_points, 5, 1);

            top.Controls.Add(new Label { Text = "Valor de referencia", Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft }, 0, 2);
            top.Controls.Add(_refValue, 1, 2);
            top.Controls.Add(_capture, 2, 2);
            top.Controls.Add(startFinishButtons, 3, 2);
            top.Controls.Add(tcPanel, 4, 2);
            top.Controls.Add(rightButtons, 5, 2);

            _grid = new DataGridView
            {
                Dock = DockStyle.Fill,
                ReadOnly = true,
                AllowUserToAddRows = false,
                AutoSizeColumnsMode = DataGridViewAutoSizeColumnsMode.Fill
            };
            _grid.Columns.Add("idx", "Indice");
            _grid.Columns.Add("ref", "Referencia");
            _grid.Columns.Add("raw", "Bruto");
            _grid.Columns.Add("samples", "Amostras");
            _grid.Columns[0].HeaderCell.ToolTipText = "Indice sequencial do ponto capturado.";
            _grid.Columns[1].HeaderCell.ToolTipText = "Valor de referencia informado para o ponto (ex: carga).";
            _grid.Columns[2].HeaderCell.ToolTipText = "Leitura bruta media recebida do DLG.";
            _grid.Columns[3].HeaderCell.ToolTipText = "Numero de amostras usadas no ponto.";

            var bottom = new Panel { Dock = DockStyle.Bottom, Height = 170, Padding = new Padding(10) };
            var infoPanel = new Panel { Dock = DockStyle.Top, Height = 60 };
            var checkPanel = new FlowLayoutPanel
            {
                Dock = DockStyle.Top,
                Height = 30,
                AutoSize = false,
                FlowDirection = FlowDirection.LeftToRight,
                WrapContents = false
            };

            _status = new Label { Dock = DockStyle.Top, Height = 20, Text = "Ocioso" };
            _error = new Label { Dock = DockStyle.Top, Height = 20, Text = "Erro (RMSE): -" };
            _result = new Label { Dock = DockStyle.Top, Height = 20, Text = "" };
            _dlgCheckStatus = new Label
            {
                AutoSize = true,
                Padding = new Padding(8, 6, 0, 0),
                ForeColor = Color.DarkRed,
                Text = "DLG: sem check"
            };
            _logBox = new TextBox
            {
                Dock = DockStyle.Fill,
                Multiline = true,
                ReadOnly = true,
                ScrollBars = ScrollBars.Vertical
            };

            infoPanel.Controls.Add(_result);
            infoPanel.Controls.Add(_error);
            infoPanel.Controls.Add(_status);
            checkPanel.Controls.Add(_checkDlg);
            checkPanel.Controls.Add(_dlgCheckStatus);
            bottom.Controls.Add(_logBox);
            bottom.Controls.Add(checkPanel);
            bottom.Controls.Add(infoPanel);

            Controls.Add(_grid);
            Controls.Add(top);
            Controls.Add(bottom);

            ResolveRuntimePaths();

            _tips = new ToolTip
            {
                AutoPopDelay = 15000,
                InitialDelay = 400,
                ReshowDelay = 200,
                ShowAlways = true
            };

            _tips.SetToolTip(_legend, "Legenda interativa com explicacoes detalhadas.");
            _tips.SetToolTip(_channel, "Canal do DLG (CH1..CH8). Deve corresponder ao canal fisico ligado ao sensor.");
            _tips.SetToolTip(_sensor, "Tipo de sensor (indice tSensor do manual). Termopar K aplica preset automatico recomendado.");
            _tips.SetToolTip(_gain, "Ganho analogico (indice iGain). Valores maiores amplificam mais o sinal.");
            _tips.SetToolTip(_lpf, "Filtro passa-baixas (LPF). Usa indices definidos no manual; 0=padrao.");
            _tips.SetToolTip(_sensPwr, "Tensao de excitacao do sensor (iSensPwr).");
            _tips.SetToolTip(_tcCjcMode, "Termopar: seleciona junta fria interna ou externa (TEDs). Campo habilitado apenas para termopar.");
            _tips.SetToolTip(_points, "Numero de pontos de calibracao (min 2).");
            _tips.SetToolTip(_refValue, "Valor de referencia do ponto atual (ex: carga aplicada).");
            _tips.SetToolTip(_start, "Inicia a sessao de calibracao e envia a configuracao.");
            _tips.SetToolTip(_capture, "Captura um ponto usando o valor de referencia atual.");
            _tips.SetToolTip(_finish, "Finaliza e grava o arquivo de calibracao.");
            _tips.SetToolTip(_cancel, "Cancela a calibracao e encerra o processo.");
            _tips.SetToolTip(_checkDlg, "Verifica se CalibraDLG.exe foi encontrado e se o DLG responde ao comando de configuracao.");
            _tips.SetToolTip(_grid, "Tabela com os pontos capturados e suas leituras.");
            _tips.SetToolTip(_error, "Erro estimado (RMSE) do ajuste linear; percentual sobre o intervalo de referencia.");
            _tips.SetToolTip(_logBox, "Log curto de runtime para diagnostico rapido.");

            UpdateThermocoupleUiState();
        }

        private static bool IsThermocoupleSensor(int tSensor)
        {
            return tSensor >= 6 && tSensor <= 13;
        }

        private void OnSensorSelectionChanged()
        {
            if (_updatingSensorUi) return;
            UpdateThermocoupleUiState();
            ApplySensorPresetIfNeeded();
        }

        private void UpdateThermocoupleUiState()
        {
            var isTc = IsThermocoupleSensor(_sensor.SelectedIndex);
            _tcCjcLabel.Enabled = isTc;
            _tcCjcMode.Enabled = isTc;
            _tcCjcLabel.Visible = true;
            _tcCjcMode.Visible = true;
        }

        private void ApplySensorPresetIfNeeded()
        {
            if (_sensor.SelectedIndex != SensorIndexTermoparK) return;
            _updatingSensorUi = true;
            try
            {
                if (GainIndex1000 >= 0 && GainIndex1000 < _gain.Items.Count) _gain.SelectedIndex = GainIndex1000;
                if (LpfUiIndexStrong >= 0 && LpfUiIndexStrong < _lpf.Items.Count) _lpf.SelectedIndex = LpfUiIndexStrong;
                if (SensPwrUiIndex1V >= 0 && SensPwrUiIndex1V < _sensPwr.Items.Count) _sensPwr.SelectedIndex = SensPwrUiIndex1V;
                _tcCjcMode.SelectedIndex = 0;
            }
            finally
            {
                _updatingSensorUi = false;
            }
            AppendLog("Preset termopar K aplicado: ganho=1000, LPF=1, excitacao=1.0V, junta fria interna.");
        }

        private string BuildConfigJson(int ch, int tSensor, int iGain, int iLpf, int iSensPwr, string outPath)
        {
            if (IsThermocoupleSensor(tSensor))
            {
                var tcCjcMode = _tcCjcMode.SelectedIndex == 1 ? 1 : 0;
                return string.Format(
                    CultureInfo.InvariantCulture,
                    "{{\"op\":\"config\",\"ch\":{0},\"tSensor\":{1},\"iGain\":{2},\"iLPF\":{3},\"iSensPwr\":{4},\"tc_cjc_mode\":{5},\"out_path\":\"{6}\"}}",
                    ch, tSensor, iGain, iLpf, iSensPwr, tcCjcMode, outPath);
            }

            return string.Format(
                CultureInfo.InvariantCulture,
                "{{\"op\":\"config\",\"ch\":{0},\"tSensor\":{1},\"iGain\":{2},\"iLPF\":{3},\"iSensPwr\":{4},\"out_path\":\"{5}\"}}",
                ch, tSensor, iGain, iLpf, iSensPwr, outPath);
        }

        private static string FindRepoRoot(params string[] seedDirs)
        {
            foreach (var seed in seedDirs)
            {
                if (string.IsNullOrWhiteSpace(seed)) continue;
                DirectoryInfo? dir = null;
                try
                {
                    dir = new DirectoryInfo(Path.GetFullPath(seed));
                }
                catch
                {
                    dir = null;
                }
                if (dir == null || !dir.Exists) continue;

                while (dir != null)
                {
                    var hasCalib = Directory.Exists(Path.Combine(dir.FullName, "CalibraDLG"));
                    var hasDlg = Directory.Exists(Path.Combine(dir.FullName, "DLG4000"));
                    if (hasCalib && hasDlg) return dir.FullName;
                    dir = dir.Parent;
                }
            }
            return string.Empty;
        }

        private static IEnumerable<string> GetCalibraExeCandidates()
        {
            var baseDir = AppContext.BaseDirectory;
            var cwd = Environment.CurrentDirectory;
            var repoRoot = FindRepoRoot(baseDir, cwd);
            var list = new List<string>();
            if (!string.IsNullOrWhiteSpace(repoRoot))
            {
                list.Add(Path.Combine(repoRoot, "CalibraDLG", "build", "Release", "CalibraDLG.exe"));
                list.Add(Path.Combine(repoRoot, "CalibraDLG", "build", "Debug", "CalibraDLG.exe"));
            }
            list.Add(Path.Combine(baseDir, "CalibraDLG.exe"));
            list.Add(Path.Combine(baseDir, "bin", "CalibraDLG.exe"));
            list.Add(Path.Combine(cwd, "CalibraDLG.exe"));
            list.Add(Path.Combine(cwd, "CalibraDLG", "build", "Release", "CalibraDLG.exe"));
            return list;
        }

        private static IEnumerable<string> GetDlgLoggerCandidates()
        {
            var baseDir = AppContext.BaseDirectory;
            var cwd = Environment.CurrentDirectory;
            var repoRoot = FindRepoRoot(baseDir, cwd);
            var list = new List<string>();
            if (!string.IsNullOrWhiteSpace(repoRoot))
            {
                list.Add(Path.Combine(repoRoot, "DLG4000", "bin", "Release", "dlg_logger_ipc.exe"));
                list.Add(Path.Combine(repoRoot, "DLG4000", "bin", "dlg_logger_ipc.exe"));
            }
            list.Add(Path.Combine(baseDir, "dlg_logger_ipc.exe"));
            list.Add(Path.Combine(baseDir, "bin", "dlg_logger_ipc.exe"));
            list.Add(Path.Combine(cwd, "DLG4000", "bin", "Release", "dlg_logger_ipc.exe"));
            list.Add(Path.Combine(cwd, "DLG4000", "bin", "dlg_logger_ipc.exe"));
            return list;
        }

        private static string FindFirstExisting(IEnumerable<string> candidates)
        {
            foreach (var c in candidates)
            {
                try
                {
                    var full = Path.GetFullPath(c);
                    if (File.Exists(full)) return full;
                }
                catch
                {
                    // ignore candidate errors
                }
            }
            return string.Empty;
        }

        private void ResolveRuntimePaths()
        {
            _calibraExePath = FindFirstExisting(GetCalibraExeCandidates());
            var dlgLogger = FindFirstExisting(GetDlgLoggerCandidates());

            if (!string.IsNullOrWhiteSpace(dlgLogger))
            {
                _calibOutDir = Path.GetDirectoryName(dlgLogger) ?? string.Empty;
            }
            else if (!string.IsNullOrWhiteSpace(_calibraExePath))
            {
                var calibDir = Path.GetDirectoryName(_calibraExePath) ?? Environment.CurrentDirectory;
                _calibOutDir = Path.Combine(calibDir, "out");
            }
            else
            {
                _calibOutDir = Path.Combine(Environment.CurrentDirectory, "out");
            }

            if (string.IsNullOrWhiteSpace(_calibraExePath))
            {
                _start.Enabled = false;
                SetStatus("CalibraDLG.exe nao encontrado.");
                SetDlgCheckState(false, "exe nao encontrado");
            }
            else if (_proc == null || _proc.HasExited)
            {
                _start.Enabled = true;
                SetStatus("Pronto para calibrar.");
                SetDlgCheckState(false, "aguardando check");
            }
        }

        private async Task StartCalibAsync()
        {
            if (_proc != null && !_proc.HasExited) return;
            AppendLog("Calibracao: iniciar solicitado.");

            if (string.IsNullOrWhiteSpace(_calibraExePath) || !File.Exists(_calibraExePath))
            {
                ResolveRuntimePaths();
            }

            if (string.IsNullOrWhiteSpace(_calibraExePath) || !File.Exists(_calibraExePath))
            {
                SetStatus("CalibraDLG.exe nao encontrado.");
                return;
            }

            _grid.Rows.Clear();
            _capturedPoints = 0;
            _expectedPoints = (int)_points.Value;
            _finish.Enabled = false;
            _finishRequested = false;
            _cancelRequested = false;
            _refPoints.Clear();
            _rawPoints.Clear();
            _error.Text = "Erro (RMSE): -";

            _proc = new Process();
            _proc.StartInfo = new ProcessStartInfo
            {
                FileName = _calibraExePath,
                Arguments = "--ipc",
                UseShellExecute = false,
                RedirectStandardInput = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true
            };
            _proc.Start();

            _stdin = _proc.StandardInput;
            _cts = new CancellationTokenSource();
            _readTask = Task.Run(() => ReadLoopAsync(_cts.Token));

            _start.Enabled = false;
            _capture.Enabled = true;
            _cancel.Enabled = true;

            var ch = _channel.SelectedIndex + 1;
            var tSensor = _sensor.SelectedIndex;
            var iGain = _gain.SelectedIndex;
            var iLpf = _lpf.SelectedIndex == 0 ? 0 : _lpf.SelectedIndex - 1;
            var iSensPwr = _sensPwr.SelectedIndex;
            var outDir = _calibOutDir;
            if (string.IsNullOrWhiteSpace(outDir))
            {
                var exeDir = Path.GetDirectoryName(_calibraExePath) ?? Environment.CurrentDirectory;
                outDir = Path.Combine(exeDir, "out");
            }
            Directory.CreateDirectory(outDir);
            var outPath = Path.Combine(outDir, $"calib_CH{ch}.json").Replace('\\', '/');

            var line = BuildConfigJson(ch, tSensor, iGain, iLpf, iSensPwr, outPath);
            SendLine(line);

            SetStatus("Configuracao enviada. Aguardando DLG...");
            AppendLog("Calibracao: configuracao enviada ao runtime.");
            await Task.Delay(50);
        }

        private async Task CheckDlgAsync()
        {
            if (_proc != null && !_proc.HasExited)
            {
                SetStatus("Finalize/cancele a calibracao antes do check.");
                AppendLog("Check DLG bloqueado: calibracao em andamento.");
                return;
            }

            _checkDlg.Enabled = false;
            AppendLog("Check DLG: iniciando.");

            try
            {
                ResolveRuntimePaths();
                if (string.IsNullOrWhiteSpace(_calibraExePath) || !File.Exists(_calibraExePath))
                {
                    SetDlgCheckState(false, "CalibraDLG.exe nao encontrado");
                    AppendLog("Check DLG: CalibraDLG.exe nao encontrado.");
                    return;
                }

                var ch = _channel.SelectedIndex + 1;
                var tSensor = _sensor.SelectedIndex;
                var iGain = _gain.SelectedIndex;
                var iLpf = _lpf.SelectedIndex == 0 ? 0 : _lpf.SelectedIndex - 1;
                var iSensPwr = _sensPwr.SelectedIndex;
                var outDir = _calibOutDir;
                if (string.IsNullOrWhiteSpace(outDir))
                {
                    var exeDir = Path.GetDirectoryName(_calibraExePath) ?? Environment.CurrentDirectory;
                    outDir = Path.Combine(exeDir, "out");
                }
                Directory.CreateDirectory(outDir);
                var outPath = Path.Combine(outDir, $"calib_CH{ch}.json").Replace('\\', '/');

                using var checkProc = new Process
                {
                    StartInfo = new ProcessStartInfo
                    {
                        FileName = _calibraExePath,
                        Arguments = "--ipc",
                        UseShellExecute = false,
                        RedirectStandardInput = true,
                        RedirectStandardOutput = true,
                        CreateNoWindow = true
                    }
                };

                checkProc.Start();

                var cfg = BuildConfigJson(ch, tSensor, iGain, iLpf, iSensPwr, outPath);
                checkProc.StandardInput.WriteLine(cfg);
                checkProc.StandardInput.Flush();

                bool ok = false;
                string detail = "timeout sem resposta";
                var deadline = DateTime.UtcNow.AddSeconds(3);

                while (DateTime.UtcNow < deadline)
                {
                    var remain = deadline - DateTime.UtcNow;
                    if (remain <= TimeSpan.Zero) break;

                    var readTask = checkProc.StandardOutput.ReadLineAsync();
                    var done = await Task.WhenAny(readTask, Task.Delay(remain));
                    if (done != readTask) break;

                    var line = readTask.Result;
                    if (line == null) break;
                    AppendLog($"Check DLG RX: {line}");

                    try
                    {
                        using var doc = JsonDocument.Parse(line);
                        var root = doc.RootElement;
                        var op = root.TryGetProperty("op", out var opProp) ? opProp.GetString() ?? "" : "";
                        if (op == "config_ok")
                        {
                            ok = true;
                            detail = "comunicacao OK";
                            break;
                        }
                        if (op == "error")
                        {
                            detail = root.TryGetProperty("message", out var msgProp)
                                ? msgProp.GetString() ?? "erro"
                                : "erro";
                            break;
                        }
                    }
                    catch
                    {
                        // Ignore non-JSON lines during check.
                    }
                }

                try
                {
                    checkProc.StandardInput.WriteLine("{\"op\":\"cancel\"}");
                    checkProc.StandardInput.Flush();
                }
                catch
                {
                    // ignore cancel write error
                }
                if (!checkProc.WaitForExit(800))
                {
                    try { checkProc.Kill(); } catch { /* ignore */ }
                }

                SetDlgCheckState(ok, detail);
                AppendLog(ok ? "Check DLG: OK." : $"Check DLG: falhou ({detail}).");
            }
            catch (Exception ex)
            {
                SetDlgCheckState(false, ex.Message);
                AppendLog($"Check DLG: excecao ({ex.Message}).");
            }
            finally
            {
                _checkDlg.Enabled = true;
            }
        }

        private async Task CapturePointAsync()
        {
            if (_pendingPoint != null) return;
            if (_proc == null || _proc.HasExited) return;

            if (!double.TryParse(_refValue.Text.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out var refVal))
            {
                SetStatus("Valor de referencia invalido (use ponto).");
                return;
            }

            _pendingPoint = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
            _capture.Enabled = false;
            _finish.Enabled = false;

            var line = string.Format(CultureInfo.InvariantCulture, "{{\"op\":\"point\",\"ref\":{0}}}", refVal);
            SendLine(line);
            SetStatus("Capturando ponto...");

            await _pendingPoint.Task;
            _pendingPoint = null;

            if (_capturedPoints >= _expectedPoints)
            {
                _capture.Enabled = false;
                _finish.Enabled = true;
                SetStatus("Todos os pontos capturados. Pronto para finalizar.");
            }
            else
            {
                _capture.Enabled = true;
                SetStatus("Ponto capturado.");
            }
        }

        private async Task ReadLoopAsync(CancellationToken token)
        {
            try
            {
                while (!token.IsCancellationRequested && _proc != null && !_proc.HasExited)
                {
                    var line = await _proc.StandardOutput.ReadLineAsync();
                    if (line == null) break;
                    HandleLine(line);
                }
            }
            catch
            {
                // ignore
            }
            finally
            {
                UI(() =>
                {
                    if (_finishRequested && _status.Text.StartsWith("Finalizando", StringComparison.Ordinal))
                    {
                        SetStatus("Concluido.");
                    }
                    else if (_cancelRequested && _status.Text.StartsWith("Cancelando", StringComparison.Ordinal))
                    {
                        SetStatus("Cancelado.");
                    }
                    if (_proc == null || _proc.HasExited)
                    {
                        _start.Enabled = true;
                        _capture.Enabled = false;
                        _finish.Enabled = false;
                        _cancel.Enabled = false;
                    }
                });
            }
        }

        private void HandleLine(string line)
        {
            try
            {
                using var doc = JsonDocument.Parse(line);
                var root = doc.RootElement;
                if (!root.TryGetProperty("op", out var opProp)) return;
                var op = opProp.GetString() ?? "";

                if (op == "config_ok")
                {
                    UI(() =>
                    {
                        SetStatus("DLG pronto.");
                        AppendLog("Runtime: DLG pronto.");
                    });
                    return;
                }

                if (op == "point_result")
                {
                    var refVal = root.GetProperty("ref").GetDouble();
                    var raw = root.GetProperty("raw").GetDouble();
                    var samples = root.GetProperty("samples").GetInt32();
                    UI(() =>
                    {
                        _capturedPoints++;
                        _grid.Rows.Add(_capturedPoints, refVal.ToString("G6", CultureInfo.InvariantCulture),
                            raw.ToString("G6", CultureInfo.InvariantCulture), samples);
                        _refPoints.Add(refVal);
                        _rawPoints.Add(raw);
                        UpdateErrorLabel();
                        AppendLog($"Ponto {_capturedPoints}/{_expectedPoints} capturado. ref={refVal:G6} bruto={raw:G6}.");
                        _pendingPoint?.TrySetResult(true);
                    });
                    return;
                }

                if (op == "done")
                {
                    var slope = root.GetProperty("slope").GetDouble();
                    var intercept = root.GetProperty("intercept").GetDouble();
                    var r2 = root.GetProperty("r2").GetDouble();
                    var outPath = root.GetProperty("out_path").GetString() ?? "";
                    UI(() =>
                    {
                        _result.Text = $"Ajuste: slope={slope:G6} intercept={intercept:G6} r2={r2:G4}  saida={outPath}";
                        _finish.Enabled = false;
                        _capture.Enabled = false;
                        _start.Enabled = true;
                        _cancel.Enabled = false;
                        SetStatus("Concluido.");
                        AppendLog("Calibracao concluida e arquivo salvo.");
                    });
                    return;
                }

                if (op == "cancelled")
                {
                    UI(() =>
                    {
                        SetStatus("Cancelado.");
                        _start.Enabled = true;
                        _capture.Enabled = false;
                        _finish.Enabled = false;
                        _cancel.Enabled = false;
                        AppendLog("Calibracao cancelada.");
                    });
                    return;
                }

                if (op == "error")
                {
                    var msg = root.TryGetProperty("message", out var m) ? m.GetString() : "error";
                    UI(() =>
                    {
                        SetStatus($"Erro: {msg}");
                        AppendLog($"Runtime erro: {msg}.");
                        _pendingPoint?.TrySetResult(true);
                    });
                }
            }
            catch
            {
                // ignore malformed lines
            }
        }

        private void SendLine(string line)
        {
            try
            {
                if (_stdin == null)
                {
                    SetStatus("Processo nao iniciado.");
                    return;
                }
                _stdin.WriteLine(line);
                _stdin.Flush();
            }
            catch
            {
                SetStatus("Falha ao enviar para CalibraDLG.");
            }
        }

        private void SetStatus(string text)
        {
            _status.Text = text;
        }

        private void AppendLog(string text)
        {
            var ts = DateTime.Now.ToString("HH:mm:ss", CultureInfo.InvariantCulture);
            var line = $"[{ts}] {text}";
            if (_logBox.TextLength > 0) _logBox.AppendText(Environment.NewLine);
            _logBox.AppendText(line);
            _logBox.SelectionStart = _logBox.TextLength;
            _logBox.ScrollToCaret();
        }

        private void SetDlgCheckState(bool ok, string detail)
        {
            _dlgCheckStatus.Text = ok ? $"DLG: OK - {detail}" : $"DLG: X - {detail}";
            _dlgCheckStatus.ForeColor = ok ? Color.DarkGreen : Color.DarkRed;
        }

        private void UI(Action action)
        {
            if (InvokeRequired) BeginInvoke(action);
            else action();
        }

        protected override void OnFormClosing(FormClosingEventArgs e)
        {
            try
            {
                _cts?.Cancel();
                if (_proc != null && !_proc.HasExited)
                {
                    _stdin?.WriteLine("{\"op\":\"cancel\"}");
                    _stdin?.Flush();
                    _proc.Kill();
                }
            }
            catch
            {
                // ignore
            }
            base.OnFormClosing(e);
        }

        private void UpdateErrorLabel()
        {
            if (_refPoints.Count < 2)
            {
                _error.Text = "Erro (RMSE): -";
                return;
            }

            var n = _refPoints.Count;
            double sumX = 0.0;
            double sumY = 0.0;
            for (int i = 0; i < n; i++)
            {
                sumX += _rawPoints[i];
                sumY += _refPoints[i];
            }
            var meanX = sumX / n;
            var meanY = sumY / n;

            double sxx = 0.0;
            double sxy = 0.0;
            for (int i = 0; i < n; i++)
            {
                var dx = _rawPoints[i] - meanX;
                var dy = _refPoints[i] - meanY;
                sxx += dx * dx;
                sxy += dx * dy;
            }

            if (Math.Abs(sxx) < 1e-12)
            {
                _error.Text = "Erro (RMSE): -";
                return;
            }

            var slope = sxy / sxx;
            var intercept = meanY - slope * meanX;

            double sumErr2 = 0.0;
            var minRef = _refPoints[0];
            var maxRef = _refPoints[0];
            for (int i = 0; i < n; i++)
            {
                var est = slope * _rawPoints[i] + intercept;
                var err = _refPoints[i] - est;
                sumErr2 += err * err;
                if (_refPoints[i] < minRef) minRef = _refPoints[i];
                if (_refPoints[i] > maxRef) maxRef = _refPoints[i];
            }

            var rmse = Math.Sqrt(sumErr2 / n);
            var span = maxRef - minRef;
            if (Math.Abs(span) > 1e-12)
            {
                var rmsePct = 100.0 * rmse / span;
                _error.Text = $"Erro (RMSE): {rmse:G6} ({rmsePct:G4}%)";
            }
            else
            {
                _error.Text = $"Erro (RMSE): {rmse:G6}";
            }
        }

        private void FinishCalib()
        {
            if (_proc == null || _proc.HasExited)
            {
                SetStatus("Processo nao iniciado.");
                return;
            }
            if (_pendingPoint != null)
            {
                SetStatus("Aguardando ponto atual.");
                return;
            }
            _finishRequested = true;
            _cancelRequested = false;
            _finish.Enabled = false;
            SendLine("{\"op\":\"finish\"}");
            SetStatus("Finalizando...");
            AppendLog("Calibracao: finalizar solicitado.");
        }

        private void CancelCalib()
        {
            _cancelRequested = true;
            _finishRequested = false;
            SendLine("{\"op\":\"cancel\"}");
            SetStatus("Cancelando...");
            AppendLog("Calibracao: cancelamento solicitado.");
        }

        private void ShowLegend()
        {
            if (_legendForm == null || _legendForm.IsDisposed)
            {
                _legendForm = new LegendForm();
            }
            _legendForm.Show(this);
            _legendForm.BringToFront();
        }

        private sealed class LegendForm : Form
        {
            private readonly ListBox _topics;
            private readonly TextBox _details;
            private readonly List<(string Title, string Body)> _items;

            public LegendForm()
            {
                Text = "Legenda interativa";
                Width = 640;
                Height = 420;

                var split = new SplitContainer
                {
                    Dock = DockStyle.Fill,
                    Orientation = Orientation.Vertical,
                    SplitterDistance = 200
                };

                _topics = new ListBox { Dock = DockStyle.Fill };
                _details = new TextBox
                {
                    Dock = DockStyle.Fill,
                    Multiline = true,
                    ReadOnly = true,
                    ScrollBars = ScrollBars.Vertical
                };

                _items = new List<(string Title, string Body)>
                {
                    ("Runtime", "O UI resolve automaticamente CalibraDLG.exe e o destino da calibracao. Nao e necessario selecionar executavel manualmente."),
                    ("Canal", "Seleciona o canal fisico do DLG (CH1..CH8). Use o canal onde o sensor esta ligado."),
                    ("Sensor", "Tipo de sensor (tSensor). O indice deve seguir o manual. Ex: ponte completa, meia ponte, ponte 1/4. Ao selecionar termopar K, a tela preenche um preset recomendado."),
                    ("Preset termopar K", "Preset aplicado ao selecionar termopar K: ganho=1000 (iGain=6), LPF=1 (mais forte suportado no fluxo atual), excitacao=1.0V e junta fria interna."),
                    ("Junta fria (termopar)", "Disponivel apenas para termopar. Define referencia de compensacao da junta fria: interna ou externa (TEDs)."),
                    ("Ganho", "Ganho analogico (iGain). Aumenta a amplitude do sinal antes da conversao. Valores altos saturam mais facil."),
                    ("Filtro LPF", "Filtro passa-baixas (LPF) no hardware. Indices definidos pelo manual; 0=padrao."),
                    ("Excitacao", "Tensao de excitacao do sensor (iSensPwr). Escolha a que o sensor suporta."),
                    ("Pontos", "Numero total de pontos de calibracao. Minimo 2 para ajuste linear."),
                    ("Valor de referencia", "Valor conhecido aplicado no sensor (ex: carga, torque, pressao). Use ponto decimal."),
                    ("Capturar ponto", "Registra o par (referencia, leitura bruta) para o ajuste."),
                    ("Finalizar", "Calcula o ajuste linear e grava o arquivo de calibracao (JSON)."),
                    ("Erro (RMSE)", "Erro estimado do ajuste linear com os pontos atuais. Percentual usa o intervalo de referencia."),
                    ("Indice", "Indice sequencial do ponto capturado na tabela."),
                    ("Referencia", "Valor de referencia informado para o ponto na tabela."),
                    ("Bruto", "Leitura bruta media do sensor naquele ponto."),
                    ("Amostras", "Quantidade de amostras utilizadas para calcular o ponto."),
                    ("Resultados", "Mostra slope/intercept/r2 e o caminho do arquivo gerado.")
                };

                foreach (var item in _items) _topics.Items.Add(item.Title);

                _topics.SelectedIndexChanged += (_, __) =>
                {
                    var idx = _topics.SelectedIndex;
                    _details.Text = idx >= 0 ? _items[idx].Body : "";
                };

                split.Panel1.Controls.Add(_topics);
                split.Panel2.Controls.Add(_details);
                Controls.Add(split);

                _topics.SelectedIndex = 0;
            }
        }
    }
}
