using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Drawing;
using System.Windows;
using GlassForge.Models;
using GlassForge.Services;

namespace GlassForge;

public partial class MainWindow : Window, INotifyPropertyChanged
{
    private readonly ProfileStore _store;
    private readonly GlassHostManager _hosts;
    private readonly WindowDiscoveryService _discovery = new();
    private readonly System.Windows.Forms.NotifyIcon _tray;
    private AppProfile? _selectedProfile;
    private bool _reallyClose;

    public ObservableCollection<AppProfile> Profiles { get; }
    public ObservableCollection<RunningApplication> RunningApps { get; } = [];
    public AppProfile? SelectedProfile { get => _selectedProfile; set { _selectedProfile = value; PropertyChanged?.Invoke(this, new(nameof(SelectedProfile))); } }
    public event PropertyChangedEventHandler? PropertyChanged;

    public MainWindow(ProfileStore store, GlassHostManager hosts)
    {
        _store = store;
        _hosts = hosts;
        Profiles = new ObservableCollection<AppProfile>(store.Load());
        SelectedProfile = Profiles.FirstOrDefault();
        DataContext = this;
        InitializeComponent();
        StartupCheckBox.IsChecked = StartupService.IsEnabled;
        RefreshRunningApplications();

        var menu = new System.Windows.Forms.ContextMenuStrip();
        menu.Items.Add("Open GlassForge", null, (_, _) => ShowFromTray());
        menu.Items.Add("Exit", null, (_, _) => ExitApplication());
        _tray = new System.Windows.Forms.NotifyIcon
        {
            Text = "GlassForge",
            Icon = SystemIcons.Application,
            Visible = true,
            ContextMenuStrip = menu
        };
        _tray.DoubleClick += (_, _) => ShowFromTray();
    }

    private void RefreshRunningApplications()
    {
        RunningApps.Clear();
        foreach (var app in _discovery.GetRunningApplications()) RunningApps.Add(app);
        StatusText.Text = $"Found {RunningApps.Count} visible applications";
    }

    private void RefreshApps(object sender, RoutedEventArgs e) => RefreshRunningApplications();
    private void ProfileSelected(object sender, System.Windows.Controls.SelectionChangedEventArgs e) { }

    private void AddRunningApp(object sender, RoutedEventArgs e)
    {
        if (RunningAppsCombo.SelectedItem is not RunningApplication app) return;
        var existing = Profiles.FirstOrDefault(profile => profile.ProcessName.Equals(app.ProcessName, StringComparison.OrdinalIgnoreCase));
        if (existing is not null) { SelectedProfile = existing; ProfilesList.SelectedItem = existing; return; }
        var profile = new AppProfile { DisplayName = app.DisplayName, ProcessName = app.ProcessName };
        Profiles.Add(profile);
        SelectedProfile = profile;
        ProfilesList.SelectedItem = profile;
        SaveProfiles();
    }

    private void SaveAndApply(object sender, RoutedEventArgs e)
    {
        if (SelectedProfile is null) return;
        if (SelectedProfile.TintColor.Length != 6 || !uint.TryParse(SelectedProfile.TintColor, System.Globalization.NumberStyles.HexNumber, null, out _))
        {
            StatusText.Text = "Tint must be a six-digit hex color, for example 181818.";
            return;
        }
        SaveProfiles();
        _hosts.Restart(SelectedProfile);
        StatusText.Text = $"Applied {SelectedProfile.DisplayName}";
    }

    private void StopProfile(object sender, RoutedEventArgs e)
    {
        if (SelectedProfile is null) return;
        _hosts.Stop(SelectedProfile.Id);
        StatusText.Text = $"Stopped {SelectedProfile.DisplayName}";
    }

    private void DeleteProfile(object sender, RoutedEventArgs e)
    {
        if (SelectedProfile is null) return;
        var remove = SelectedProfile;
        _hosts.Stop(remove.Id);
        Profiles.Remove(remove);
        SelectedProfile = Profiles.FirstOrDefault();
        SaveProfiles();
    }

    private void StartupChanged(object sender, RoutedEventArgs e)
    {
        try { StartupService.SetEnabled(StartupCheckBox.IsChecked == true); StatusText.Text = "Startup preference saved"; }
        catch (Exception exception) { StatusText.Text = exception.Message; }
    }

    private void SaveProfiles() => _store.Save(Profiles);

    protected override void OnClosing(CancelEventArgs e)
    {
        if (!_reallyClose)
        {
            e.Cancel = true;
            Hide();
            StatusText.Text = "GlassForge is running in the notification area";
            return;
        }
        _tray.Visible = false;
        _tray.Dispose();
        base.OnClosing(e);
    }

    private void ShowFromTray()
    {
        Show();
        WindowState = WindowState.Normal;
        Activate();
    }

    private void ExitApplication()
    {
        _reallyClose = true;
        Close();
        System.Windows.Application.Current.Shutdown();
    }
}
