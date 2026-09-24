using System.Windows;
using GlassForge.Services;

namespace GlassForge;

public partial class App : System.Windows.Application
{
    private GlassHostManager? _hosts;
    private Mutex? _singleInstance;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        // Two instances fight over the same target windows and leave them translucent on exit.
        _singleInstance = new Mutex(true, @"Local\GlassForge.SingleInstance", out var firstInstance);
        if (!firstInstance)
        {
            if (!e.Args.Contains("--background", StringComparer.OrdinalIgnoreCase))
                System.Windows.MessageBox.Show("GlassForge is already running. Open it from the notification area.", "GlassForge");
            Shutdown();
            return;
        }
        WindowStyleLedger.RestoreOrphans();
        var store = new ProfileStore();
        _hosts = new GlassHostManager();
        var window = new MainWindow(store, _hosts);
        if (!e.Args.Contains("--no-effects", StringComparer.OrdinalIgnoreCase))
            foreach (var profile in store.Load().Where(profile => profile.Enabled)) _hosts.Start(profile);
        if (!e.Args.Contains("--background", StringComparer.OrdinalIgnoreCase)) window.Show();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        _hosts?.Dispose();
        _singleInstance?.Dispose();
        base.OnExit(e);
    }
}
