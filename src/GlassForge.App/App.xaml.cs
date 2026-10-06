using System.Windows;
using GlassForge.Services;

namespace GlassForge;

public partial class App : System.Windows.Application
{
    private GlassHostManager? _hosts;
    private InstanceCoordinator? _instance;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        // Developer check of the app diagnosis: --diagnose name[,name...] writes %LOCALAPPDATA%\GlassForge\diagnosis.txt
        // and exits, before the single-instance handshake so a running GlassForge is left alone. Read-only.
        var diagnose = Array.IndexOf(e.Args, "--diagnose");
        if (diagnose >= 0 && diagnose + 1 < e.Args.Length)
        {
            AppDiagnosisService.WriteReport(e.Args[diagnose + 1].Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries));
            Shutdown();
            return;
        }
        // Two instances fight over the same target windows and leave them translucent on exit.
        var (instance, outcome) = InstanceCoordinator.Acquire();
        if (instance is null)
        {
            if (!e.Args.Contains("--background", StringComparer.OrdinalIgnoreCase))
                System.Windows.MessageBox.Show(outcome == InstanceCoordinator.Outcome.AlreadyRunning
                    ? "GlassForge is already running. Open it from the notification area."
                    : "Another copy of GlassForge is running and couldn't be closed. Exit it from its tray icon and try again.", "GlassForge");
            Shutdown();
            return;
        }
        _instance = instance;
        WindowStyleLedger.RestoreOrphans();
        StartupService.RepointIfStale();
        var store = new ProfileStore();
        _hosts = new GlassHostManager();
        var window = new MainWindow(store, _hosts);
        _instance.ListenForExitRequests(() => Dispatcher.BeginInvoke(window.ExitApplication));
        if (!e.Args.Contains("--no-effects", StringComparer.OrdinalIgnoreCase))
            foreach (var profile in store.Load().Where(profile => profile.Enabled)) _hosts.Start(profile);
        if (!e.Args.Contains("--background", StringComparer.OrdinalIgnoreCase)) window.Show();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        _hosts?.Dispose();
        _instance?.Dispose();
        base.OnExit(e);
    }
}
