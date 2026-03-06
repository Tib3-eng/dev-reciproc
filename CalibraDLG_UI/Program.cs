/*
Program.cs
----------
Ponto de entrada da aplicacao WinForms CalibraDLG_UI.

Objetivo:
- Inicializar o contexto grafico do .NET (ApplicationConfiguration).
- Abrir a janela principal MainForm, que controla o fluxo de calibracao IPC.

Observacao:
- Toda logica de negocio (IPC, validacao de pontos, UI de calibracao)
  fica em MainForm.cs. Este arquivo somente sobe a aplicacao.
*/

using System;
using System.Windows.Forms;

namespace CalibraDLG_UI
{
    internal static class Program
    {
        [STAThread]
        static void Main()
        {
            ApplicationConfiguration.Initialize();
            Application.Run(new MainForm());
        }
    }
}
