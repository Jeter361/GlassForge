using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Drawing;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using WpfButton = System.Windows.Controls.Button;
using GlassForge.Models;
using GlassForge.Services;

namespace GlassForge;

public partial class MainWindow : Window, INotifyPropertyChanged
{
    private readonly ProfileStore _store;
    private readonly GlassHostManager _hosts;
    private readonly WindowDiscoveryService _discovery = new();
    private readonly System.Windows.Forms.NotifyIcon _tray;
    private readonly DispatcherTimer _appRefreshTimer = new() { Interval = TimeSpan.FromSeconds(2) };
    // Edits apply live, once the user pauses: a slider drag or a typed name restarts the session once, not per tick.
    private readonly DispatcherTimer _applyTimer = new() { Interval = TimeSpan.FromMilliseconds(400) };
    private readonly HashSet<AppProfile> _pending = [];
    private IReadOnlyList<RunningApplication> _allRunningApps = [];
    private AppProfile? _selectedProfile;
    private bool _reallyClose;

    // Changes to these don't alter the glass, so they neither save nor restart a session.
    private static readonly HashSet<string> DisplayOnly =
    [
        nameof(AppProfile.Icon), nameof(AppProfile.HasIcon), nameof(AppProfile.Initial), nameof(AppProfile.OpacityLabel),
        nameof(AppProfile.TintLabel), nameof(AppProfile.TintHex), nameof(AppProfile.ModeLabel), nameof(AppProfile.ExecutablePath),
        nameof(AppProfile.IsExplorer), nameof(AppProfile.IsTerminal), nameof(AppProfile.IsSettings), nameof(AppProfile.IsOffice),
        nameof(AppProfile.IsNotepad), nameof(AppProfile.HasHook), nameof(AppProfile.HookSummary), nameof(AppProfile.HookWarning),
        nameof(AppProfile.OfficeDisplayName), nameof(AppProfile.OfficeExeName)
    ];

    public ObservableCollection<AppProfile> Profiles { get; }
    public ObservableCollection<RunningApplication> RunningApps { get; } = [];
    public AppProfile? SelectedProfile { get => _selectedProfile; set { _selectedProfile = value; PropertyChanged?.Invoke(this, new(nameof(SelectedProfile))); } }
    public event PropertyChangedEventHandler? PropertyChanged;

    public MainWindow(ProfileStore store, GlassHostManager hosts)
    {
        _store = store;
        _hosts = hosts;
        Profiles = new ObservableCollection<AppProfile>(store.Load());
        // Older versions named profiles after the window title ("notes.txt - Notepad"); show the app's own name.
        var renamed = false;
        foreach (var profile in Profiles)
        {
            if (AppIconService.FriendlyName(profile.ExecutablePath) is { } name && name != profile.DisplayName)
            {
                profile.DisplayName = name;
                renamed = true;
            }
            profile.PropertyChanged += ProfileChanged;
        }
        if (renamed) SaveProfiles();
        SelectedProfile = Profiles.FirstOrDefault();
        DataContext = this;
        InitializeComponent();
        SourceInitialized += (_, _) =>
        {
            WindowBackdropService.Apply(this);
            WindowBackdropService.KeepGlassWhenInactive(this);
        };
        StartupCheckBox.IsChecked = StartupService.IsEnabled;
        _applyTimer.Tick += (_, _) => ApplyPending();
        _appRefreshTimer.Tick += (_, _) => RefreshRunningApplications();
        if (!TargetWindowStyler.SupportsSystemBackdrop) NativeUnsupportedText.Visibility = Visibility.Visible;
        if (!HookLibrary.IsAvailable) ExplorerHookMissingText.Visibility = Visibility.Visible;

        var menu = new System.Windows.Forms.ContextMenuStrip();
        menu.Items.Add("Open GlassForge", null, (_, _) => ShowFromTray());
        menu.Items.Add("Exit", null, (_, _) => ExitApplication());
        _tray = new System.Windows.Forms.NotifyIcon
        {
            Text = "GlassForge",
            Icon = LoadTrayIcon(),
            Visible = true,
            ContextMenuStrip = menu
        };
        _tray.DoubleClick += (_, _) => ShowFromTray();
    }

    // Pick the small-icon frame for the current DPI so the tray icon stays crisp instead of downscaling 32px.
    private static System.Drawing.Icon LoadTrayIcon()
    {
        var resource = System.Windows.Application.GetResourceStream(new Uri("pack://application:,,,/Assets/GlassForge.ico"));
        if (resource is null) return SystemIcons.Application;
        using var stream = resource.Stream;
        return new System.Drawing.Icon(stream, System.Windows.Forms.SystemInformation.SmallIconSize);
    }

    private void ProfileChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (sender is not AppProfile profile || e.PropertyName is null || DisplayOnly.Contains(e.PropertyName)) return;
        _pending.Add(profile);
        _applyTimer.Stop();
        _applyTimer.Start();
    }

    private void ApplyPending()
    {
        _applyTimer.Stop();
        SaveProfiles();
        foreach (var profile in _pending.Where(Profiles.Contains)) _hosts.Restart(profile);
        _pending.Clear();
    }

    // The add-app picker: running apps, minus the ones that already have glass. Only scanned while it's open.
    private void ShowAddApp(object sender, RoutedEventArgs e) => AddAppPopup.IsOpen = true;

    private void AddAppOpened(object? sender, EventArgs e)
    {
        AppSearchBox.Text = "";
        RefreshRunningApplications();
        _appRefreshTimer.Start();
        AddAppPopup.Closed += StopRefreshing;
        Dispatcher.BeginInvoke(() => AppSearchBox.Focus(), DispatcherPriority.Input);
    }

    private void StopRefreshing(object? sender, EventArgs e)
    {
        AddAppPopup.Closed -= StopRefreshing;
        _appRefreshTimer.Stop();
    }

    private void RefreshRunningApplications()
    {
        var added = Profiles.Select(profile => profile.ProcessName).ToHashSet(StringComparer.OrdinalIgnoreCase);
        _allRunningApps = _discovery.GetRunningApplications()
            .Where(app => !added.Contains(app.ProcessName) && !app.ProcessName.Equals("ApplicationFrameHost", StringComparison.OrdinalIgnoreCase))
            .OrderBy(app => app.Name, StringComparer.CurrentCultureIgnoreCase)
            .ToArray();
        ApplyAppFilter();
    }

    private void ApplyAppFilter()
    {
        var query = AppSearchBox?.Text.Trim() ?? string.Empty;
        var filtered = string.IsNullOrWhiteSpace(query)
            ? _allRunningApps
            : _allRunningApps.Where(app => app.Name.Contains(query, StringComparison.OrdinalIgnoreCase)
                || app.ProcessName.Contains(query, StringComparison.OrdinalIgnoreCase)).ToArray();
        NoRunningAppsText.Visibility = _allRunningApps.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        var nextKeys = filtered.Select(app => app.ProcessName).ToArray();
        if (RunningApps.Select(app => app.ProcessName).SequenceEqual(nextKeys, StringComparer.OrdinalIgnoreCase)) return;
        RunningApps.Clear();
        foreach (var app in filtered) RunningApps.Add(app);
    }

    private void AppSearchChanged(object sender, TextChangedEventArgs e) => ApplyAppFilter();

    private void AddRunningApp(object sender, RoutedEventArgs e)
    {
        if (sender is not WpfButton { DataContext: RunningApplication app }) return;
        AddAppPopup.IsOpen = false;
        var profile = new AppProfile { DisplayName = app.Name, ProcessName = app.ProcessName, ExecutablePath = app.ExecutablePath ?? "" };
        profile.PropertyChanged += ProfileChanged;
        Profiles.Add(profile);
        SelectedProfile = profile;
        SaveProfiles();
        _hosts.Restart(profile);
    }

    private void ProfileSelected(object sender, SelectionChangedEventArgs e)
    {
        if (AdvancedPanel is null || AdvancedChevron is null) return; // the first selection lands while the XAML loads
        AdvancedPanel.Visibility = Visibility.Collapsed;
        AdvancedChevron.Text = "";
    }

    private void ToggleAdvanced(object sender, RoutedEventArgs e)
    {
        var show = AdvancedPanel.Visibility != Visibility.Visible;
        AdvancedPanel.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        AdvancedChevron.Text = show ? "" : "";
    }

    private void ShowSettings(object sender, RoutedEventArgs e) => SettingsPopup.IsOpen = true;

    // Click, not Checked: Checked also fires when a Native profile is merely shown, and would undo the user's choice.
    private void NativeModeChosen(object sender, RoutedEventArgs e)
    {
        if (SelectedProfile is { DrawsOwnTitleBar: true }) SelectedProfile.ExtendIntoClient = true;
    }

    private void SetTintPreset(object sender, RoutedEventArgs e)
    {
        if (SelectedProfile is null || sender is not WpfButton { Tag: string color }) return;
        SelectedProfile.TintColor = color;
    }

    private void ResetAppearance(object sender, RoutedEventArgs e) => SelectedProfile?.ResetAppearance();

    private void ChooseTintColor(object sender, RoutedEventArgs e)
    {
        if (SelectedProfile is null) return;
        using var dialog = new System.Windows.Forms.ColorDialog { FullOpen = true, AnyColor = true };
        if (uint.TryParse(SelectedProfile.TintColor, System.Globalization.NumberStyles.HexNumber, null, out var rgb))
            dialog.Color = System.Drawing.Color.FromArgb((int)(rgb | 0xFF000000));
        if (dialog.ShowDialog() != System.Windows.Forms.DialogResult.OK) return;
        SelectedProfile.TintColor = $"{dialog.Color.R:X2}{dialog.Color.G:X2}{dialog.Color.B:X2}";
    }

    private void DeleteProfile(object sender, RoutedEventArgs e)
    {
        if (SelectedProfile is null) return;
        var remove = SelectedProfile;
        remove.PropertyChanged -= ProfileChanged;
        _pending.Remove(remove);
        _hosts.Stop(remove.Id);
        Profiles.Remove(remove);
        SelectedProfile = Profiles.FirstOrDefault();
        SaveProfiles();
    }

    private void StartupChanged(object sender, RoutedEventArgs e)
    {
        try { StartupService.SetEnabled(StartupCheckBox.IsChecked == true); }
        catch (Exception) { StartupCheckBox.IsChecked = StartupService.IsEnabled; }
    }

    private void SaveProfiles() => _store.Save(Profiles);

    private void MinimizeClicked(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;

    private void MaximizeClicked(object sender, RoutedEventArgs e)
        => WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;

    private void CloseClicked(object sender, RoutedEventArgs e) => Close();

    private void WindowStateChanged(object? sender, EventArgs e)
    {
        // WindowChrome windows overhang the monitor by the resize border when maximized.
        var maximized = WindowState == WindowState.Maximized;
        RootGrid.Margin = maximized ? new Thickness(7) : new Thickness(0);
        WindowBackdropService.Apply(this); // WPF can restore the system menu style on state changes
        MaximizeGlyph.Text = maximized ? "" : "";
    }

    protected override void OnClosing(CancelEventArgs e)
    {
        if (!_reallyClose)
        {
            e.Cancel = true;
            if (_applyTimer.IsEnabled) ApplyPending();
            Hide();
            return;
        }
        if (_applyTimer.IsEnabled) ApplyPending();
        _tray.Visible = false;
        _tray.Dispose();
        _appRefreshTimer.Stop();
        base.OnClosing(e);
    }

    private void ShowFromTray()
    {
        Show();
        WindowState = WindowState.Normal;
        Activate();
    }

    internal void ExitApplication()
    {
        _reallyClose = true;
        Close();
        System.Windows.Application.Current.Shutdown();
    }
}
