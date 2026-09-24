using System.Windows;
using GlassForge.Services;

namespace GlassForge;

public partial class App : System.Windows.Application
{
    private GlassHostManager? _hosts;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
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
        base.OnExit(e);
    }
}
