using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Drawing;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Threading;
using WpfButton = System.Windows.Controls.Button;
using WpfTextBox = System.Windows.Controls.TextBox;
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
    private IReadOnlyList<RunningApplication> _allRunningApps = [];
    private AppProfile? _selectedProfile;
    private bool _reallyClose;
    private bool _updatingColor;

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
        UpdateColorControls();
        _appRefreshTimer.Tick += (_, _) => RefreshRunningApplications(false);
        _appRefreshTimer.Start();

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

    private void RefreshRunningApplications(bool announce = true)
    {
        _allRunningApps = _discovery.GetRunningApplications();
        ApplyAppFilter();
        if (announce) StatusText.Text = $"Watching {RunningApps.Count} visible applications";
    }

    private void ApplyAppFilter()
    {
        var query = AppSearchBox?.Text.Trim() ?? string.Empty;
        var filtered = string.IsNullOrWhiteSpace(query)
            ? _allRunningApps
            : _allRunningApps.Where(app => app.DisplayName.Contains(query, StringComparison.OrdinalIgnoreCase)
                || app.ProcessName.Contains(query, StringComparison.OrdinalIgnoreCase)).ToArray();
        var nextKeys = filtered.Select(app => app.ProcessName).ToArray();
        if (RunningApps.Select(app => app.ProcessName).SequenceEqual(nextKeys, StringComparer.OrdinalIgnoreCase)) return;
        RunningApps.Clear();
        foreach (var app in filtered) RunningApps.Add(app);
    }

    private void AppSearchChanged(object sender, TextChangedEventArgs e) => ApplyAppFilter();

    private void ProfileSelected(object sender, SelectionChangedEventArgs e) => UpdateColorControls();

    private void AddRunningAppCard(object sender, RoutedEventArgs e)
    {
        if (sender is not WpfButton { DataContext: RunningApplication app }) return;
        var existing = Profiles.FirstOrDefault(profile => profile.ProcessName.Equals(app.ProcessName, StringComparison.OrdinalIgnoreCase));
        if (existing is not null) { SelectedProfile = existing; ProfilesList.SelectedItem = existing; return; }
        var profile = new AppProfile { DisplayName = app.DisplayName, ProcessName = app.ProcessName };
        Profiles.Add(profile);
        SelectedProfile = profile;
        ProfilesList.SelectedItem = profile;
        SaveProfiles();
        StatusText.Text = $"Added {profile.DisplayName}";
    }

    private void SetTintPreset(object sender, RoutedEventArgs e)
    {
        if (SelectedProfile is null || sender is not WpfButton { Tag: string color }) return;
        SelectedProfile.TintColor = color;
        UpdateColorControls();
    }

    private void ChooseTintColor(object sender, RoutedEventArgs e)
    {
        if (SelectedProfile is null) return;
        using var dialog = new System.Windows.Forms.ColorDialog { FullOpen = true, AnyColor = true };
        if (TryParseColor(SelectedProfile.TintColor, out var red, out var green, out var blue))
            dialog.Color = System.Drawing.Color.FromArgb(red, green, blue);
        if (dialog.ShowDialog() != System.Windows.Forms.DialogResult.OK) return;
        SelectedProfile.TintColor = $"{dialog.Color.R:X2}{dialog.Color.G:X2}{dialog.Color.B:X2}";
        UpdateColorControls();
    }

    private void TintHexChanged(object sender, TextChangedEventArgs e)
    {
        if (_updatingColor || SelectedProfile is null || sender is not WpfTextBox box) return;
        var value = box.Text.Trim().TrimStart('#');
        if (!TryParseColor(value, out _, out _, out _)) return;
        SelectedProfile.TintColor = value;
        UpdateColorControls();
    }

    private void TintChannelChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        if (_updatingColor || SelectedProfile is null || RedSlider is null || GreenSlider is null || BlueSlider is null) return;
        var red = (byte)Math.Round(RedSlider.Value);
        var green = (byte)Math.Round(GreenSlider.Value);
        var blue = (byte)Math.Round(BlueSlider.Value);
        SelectedProfile.TintColor = $"{red:X2}{green:X2}{blue:X2}";
        UpdateColorControls();
    }

    private void UpdateColorControls()
    {
        if (ColorPreview is null || RedSlider is null || GreenSlider is null || BlueSlider is null
            || RedValue is null || GreenValue is null || BlueValue is null || SelectedProfile is null) return;
        if (!TryParseColor(SelectedProfile.TintColor, out var red, out var green, out var blue)) return;
        _updatingColor = true;
        try
        {
            RedSlider.Value = red;
            GreenSlider.Value = green;
            BlueSlider.Value = blue;
            RedValue.Text = red.ToString();
            GreenValue.Text = green.ToString();
            BlueValue.Text = blue.ToString();
            ColorPreview.Background = new SolidColorBrush(System.Windows.Media.Color.FromRgb(red, green, blue));
        }
        finally { _updatingColor = false; }
    }

    private static bool TryParseColor(string value, out byte red, out byte green, out byte blue)
    {
        red = green = blue = 0;
        value = value.Trim().TrimStart('#');
        if (value.Length != 6 || !uint.TryParse(value, System.Globalization.NumberStyles.HexNumber, null, out var rgb)) return false;
        red = (byte)(rgb >> 16);
        green = (byte)(rgb >> 8);
        blue = (byte)rgb;
        return true;
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
        RefreshRunningApplications();
        StatusText.Text = $"Removed {remove.DisplayName}; its original window style was restored";
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
        _appRefreshTimer.Stop();
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
