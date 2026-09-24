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
