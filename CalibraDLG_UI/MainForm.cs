using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace CalibraDLG_UI
{
    public sealed class MainForm : Form
    {
        private readonly TextBox _exePath;
        private readonly Button _browseExe;
        private readonly ComboBox _channel;
        private readonly ComboBox _sensor;
        private readonly ComboBox _gain;
        private readonly ComboBox _lpf;
        private readonly ComboBox _sensPwr;
        private readonly NumericUpDown _points;
        private readonly TextBox _refValue;
        private readonly Button _start;
        private readonly Button _capture;
        private readonly Button _finish;
        private readonly Button _cancel;
        private readonly Button _legend;
        private readonly DataGridView _grid;
        private readonly Label _status;
        private readonly Label _result;
        private readonly Label _error;
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

        public MainForm()
        {
            Text = "CalibraDLG - Calibracao";
            Width = 980;
            Height = 680;

            var top = new TableLayoutPanel
            {
                Dock = DockStyle.Top,
                Height = 150,
                ColumnCount = 6,
                RowCount = 4,
                Padding = new Padding(10),
                AutoSize = false
            };
            top.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 130));
            top.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 45));
            top.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 120));
            top.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 35));
            top.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 120));
            top.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 20));
            top.RowStyles.Add(new RowStyle(SizeType.Absolute, 32));
            top.RowStyles.Add(new RowStyle(SizeType.Absolute, 32));
            top.RowStyles.Add(new RowStyle(SizeType.Absolute, 32));
            top.RowStyles.Add(new RowStyle(SizeType.Absolute, 36));

            _exePath = new TextBox { Dock = DockStyle.Fill };
            _browseExe = new Button { Text = "Procurar", Dock = DockStyle.Fill };
            _browseExe.Click += (_, __) => BrowseExe();

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
            _gain.SelectedIndex = 4;

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
            _sensPwr.SelectedIndex = 0;

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

            _start.Click += async (_, __) => await StartCalibAsync();
            _capture.Click += async (_, __) => await CapturePointAsync();
            _finish.Click += (_, __) => FinishCalib();
            _cancel.Click += (_, __) => CancelCalib();
            _legend.Click += (_, __) => ShowLegend();

            top.Controls.Add(new Label { Text = "Executavel CalibraDLG", Dock = DockStyle.Fill, TextAlign = System.Drawing.ContentAlignment.MiddleLeft }, 0, 0);
            top.Controls.Add(_exePath, 1, 0);
            top.Controls.Add(_browseExe, 4, 0);
            top.Controls.Add(_legend, 5, 0);
            top.SetColumnSpan(_exePath, 3);

            top.Controls.Add(new Label { Text = "Canal", Dock = DockStyle.Fill, TextAlign = System.Drawing.ContentAlignment.MiddleLeft }, 0, 1);
            top.Controls.Add(_channel, 1, 1);
            top.Controls.Add(new Label { Text = "Sensor", Dock = DockStyle.Fill, TextAlign = System.Drawing.ContentAlignment.MiddleLeft }, 2, 1);
            top.Controls.Add(_sensor, 3, 1);
            top.Controls.Add(new Label { Text = "Ganho", Dock = DockStyle.Fill, TextAlign = System.Drawing.ContentAlignment.MiddleLeft }, 4, 1);
            top.Controls.Add(_gain, 5, 1);

            top.Controls.Add(new Label { Text = "Filtro LPF", Dock = DockStyle.Fill, TextAlign = System.Drawing.ContentAlignment.MiddleLeft }, 0, 2);
            top.Controls.Add(_lpf, 1, 2);
            top.Controls.Add(new Label { Text = "Excitacao", Dock = DockStyle.Fill, TextAlign = System.Drawing.ContentAlignment.MiddleLeft }, 2, 2);
            top.Controls.Add(_sensPwr, 3, 2);
            top.Controls.Add(new Label { Text = "Pontos", Dock = DockStyle.Fill, TextAlign = System.Drawing.ContentAlignment.MiddleLeft }, 4, 2);
            top.Controls.Add(_points, 5, 2);

            top.Controls.Add(new Label { Text = "Valor de referencia", Dock = DockStyle.Fill, TextAlign = System.Drawing.ContentAlignment.MiddleLeft }, 0, 3);
            top.Controls.Add(_refValue, 1, 3);
            top.Controls.Add(_capture, 2, 3);
            top.Controls.Add(_start, 3, 3);
            top.Controls.Add(_finish, 4, 3);
            top.Controls.Add(_cancel, 5, 3);

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

            var bottom = new Panel { Dock = DockStyle.Bottom, Height = 90, Padding = new Padding(10) };
            _status = new Label { Dock = DockStyle.Top, Height = 20, Text = "Ocioso" };
            _error = new Label { Dock = DockStyle.Top, Height = 20, Text = "Erro (RMSE): -" };
            _result = new Label { Dock = DockStyle.Top, Height = 20, Text = "" };
            bottom.Controls.Add(_result);
            bottom.Controls.Add(_error);
            bottom.Controls.Add(_status);

            Controls.Add(_grid);
            Controls.Add(top);
            Controls.Add(bottom);

            SetDefaultExePath();

            _tips = new ToolTip
            {
                AutoPopDelay = 15000,
                InitialDelay = 400,
                ReshowDelay = 200,
                ShowAlways = true
            };

            _tips.SetToolTip(_exePath, "Caminho para CalibraDLG.exe. O UI chama este executavel via IPC.");
            _tips.SetToolTip(_browseExe, "Localize o executavel CalibraDLG.exe.");
            _tips.SetToolTip(_legend, "Legenda interativa com explicacoes detalhadas.");
            _tips.SetToolTip(_channel, "Canal do DLG (CH1..CH8). Deve corresponder ao canal fisico ligado ao sensor.");
            _tips.SetToolTip(_sensor, "Tipo de sensor (indice tSensor do manual). Mantem compatibilidade com o hardware.");
            _tips.SetToolTip(_gain, "Ganho analogico (indice iGain). Valores maiores amplificam mais o sinal.");
            _tips.SetToolTip(_lpf, "Filtro passa-baixas (LPF). Usa indices definidos no manual; 0=padrao.");
            _tips.SetToolTip(_sensPwr, "Tensao de excitacao do sensor (iSensPwr).");
            _tips.SetToolTip(_points, "Numero de pontos de calibracao (min 2).");
            _tips.SetToolTip(_refValue, "Valor de referencia do ponto atual (ex: carga aplicada).");
            _tips.SetToolTip(_start, "Inicia a sessao de calibracao e envia a configuracao.");
            _tips.SetToolTip(_capture, "Captura um ponto usando o valor de referencia atual.");
            _tips.SetToolTip(_finish, "Finaliza e grava o arquivo de calibracao.");
            _tips.SetToolTip(_cancel, "Cancela a calibracao e encerra o processo.");
            _tips.SetToolTip(_grid, "Tabela com os pontos capturados e suas leituras.");
            _tips.SetToolTip(_error, "Erro estimado (RMSE) do ajuste linear; percentual sobre o intervalo de referencia.");
        }

        private void SetDefaultExePath()
        {
            var root = Environment.CurrentDirectory;
            var rel1 = Path.Combine(root, "build", "CalibraDLG", "Release", "CalibraDLG.exe");
            var rel2 = Path.Combine(root, "build", "CalibraDLG", "Debug", "CalibraDLG.exe");
            if (File.Exists(rel1)) _exePath.Text = rel1;
            else if (File.Exists(rel2)) _exePath.Text = rel2;
        }

        private void BrowseExe()
        {
            using var dlg = new OpenFileDialog
            {
                Filter = "CalibraDLG.exe|CalibraDLG.exe|Executables (*.exe)|*.exe",
                Title = "Selecionar CalibraDLG.exe"
            };
            if (dlg.ShowDialog(this) == DialogResult.OK)
            {
                _exePath.Text = dlg.FileName;
            }
        }

        private async Task StartCalibAsync()
        {
            if (_proc != null && !_proc.HasExited) return;

            var exe = _exePath.Text.Trim();
            if (string.IsNullOrEmpty(exe) || !File.Exists(exe))
            {
                SetStatus("Caminho do executavel invalido.");
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
                FileName = exe,
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
            var exeDir = Path.GetDirectoryName(exe) ?? Environment.CurrentDirectory;
            var outDir = Path.Combine(exeDir, "out");
            Directory.CreateDirectory(outDir);
            var outPath = Path.Combine(outDir, $"calib_CH{ch}.json");

            var line = string.Format(CultureInfo.InvariantCulture,
                "{{\"op\":\"config\",\"ch\":{0},\"tSensor\":{1},\"iGain\":{2},\"iLPF\":{3},\"iSensPwr\":{4},\"out_path\":\"{5}\"}}",
                ch, tSensor, iGain, iLpf, iSensPwr, outPath);
            SendLine(line);

            SetStatus("Configuracao enviada. Aguardando DLG...");
            await Task.Delay(50);
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
                    UI(() => SetStatus("DLG pronto."));
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
                    });
                    return;
                }

                if (op == "error")
                {
                    var msg = root.TryGetProperty("message", out var m) ? m.GetString() : "error";
                    UI(() =>
                    {
                        SetStatus($"Erro: {msg}");
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
        }

        private void CancelCalib()
        {
            _cancelRequested = true;
            _finishRequested = false;
            SendLine("{\"op\":\"cancel\"}");
            SetStatus("Cancelando...");
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
                    ("Executavel", "Arquivo CalibraDLG.exe que realiza a calibracao via IPC. O UI apenas orquestra."),
                    ("Canal", "Seleciona o canal fisico do DLG (CH1..CH8). Use o canal onde o sensor esta ligado."),
                    ("Sensor", "Tipo de sensor (tSensor). O indice deve seguir o manual. Ex: ponte completa, meia ponte, ponte 1/4."),
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
