using System.ComponentModel;
using System.IO;
using System.Runtime.CompilerServices;
using System.Text.Json.Serialization;

namespace GlassForge.Models;

/// <summary>How GlassForge puts glass behind the app.</summary>
[JsonConverter(typeof(JsonStringEnumConverter<GlassMode>))]
public enum GlassMode
{
    /// <summary>A GlassForge acrylic window tracks the app, which is made translucent. Works with any app.</summary>
    Host,
    /// <summary>Windows draws the backdrop on the app's own frame. Zero overhead, but only shows where the app doesn't paint.</summary>
    Native
}

[JsonConverter(typeof(JsonStringEnumConverter<CornerStyle>))]
public enum CornerStyle { Default, Round, RoundSmall, Square }

[JsonConverter(typeof(JsonStringEnumConverter<TitleBarTheme>))]
public enum TitleBarTheme { Default, Dark, Light }

public sealed class AppProfile : INotifyPropertyChanged
{
    public const byte DefaultWindowOpacity = 195;
    public const byte DefaultTintOpacity = 32;
    public const string DefaultTintColor = "181818";
    private string _displayName = "New profile";
    private string _processName = "";
    private bool _enabled = true;
    private byte _windowOpacity = DefaultWindowOpacity;
    private byte _tintOpacity = DefaultTintOpacity;
    private string _tintColor = DefaultTintColor;
    private bool _pauseWhileMediaPlays;
    private GlassMode _mode;
    private bool _extendIntoClient;
    private CornerStyle _corners;
    private TitleBarTheme _titleBar;
    private bool _keepGlassWhenUnfocused = true;
    private bool _explorerGlass;

    public Guid Id { get; set; } = Guid.NewGuid();
    public string DisplayName { get => _displayName; set => Set(ref _displayName, value); }
    public string ProcessName { get => _processName; set => Set(ref _processName, Path.GetFileNameWithoutExtension(value.Trim())); }
    /// <summary>
    /// Experimental, File Explorer only: loads GlassForge's hook into Explorer so its own backgrounds, scrollbars and
    /// text are redrawn for glass. The one GlassForge feature that injects code; off unless the user opts in.
    /// </summary>
    public bool ExplorerGlass { get => _explorerGlass; set => Set(ref _explorerGlass, value); }
    [JsonIgnore] public bool IsExplorer => string.Equals(ProcessName, "explorer", StringComparison.OrdinalIgnoreCase);
    public bool Enabled { get => _enabled; set => Set(ref _enabled, value); }
    public byte WindowOpacity { get => _windowOpacity; set => Set(ref _windowOpacity, value); }
    public byte TintOpacity { get => _tintOpacity; set => Set(ref _tintOpacity, value); }
    public string TintColor { get => _tintColor; set => Set(ref _tintColor, value.Trim().TrimStart('#').ToUpperInvariant()); }
    public bool PauseWhileMediaPlays { get => _pauseWhileMediaPlays; set => Set(ref _pauseWhileMediaPlays, value); }
    public GlassMode Mode { get => _mode; set => Set(ref _mode, value); }
    public bool ExtendIntoClient { get => _extendIntoClient; set => Set(ref _extendIntoClient, value); }
    public CornerStyle Corners { get => _corners; set => Set(ref _corners, value); }
    public TitleBarTheme TitleBar { get => _titleBar; set => Set(ref _titleBar, value); }
    public bool KeepGlassWhenUnfocused { get => _keepGlassWhenUnfocused; set => Set(ref _keepGlassWhenUnfocused, value); }

    [JsonIgnore] public string OpacityLabel => $"{Math.Round(WindowOpacity / 255d * 100)}%";
    [JsonIgnore] public string Initial => string.IsNullOrWhiteSpace(DisplayName) ? "?" : DisplayName.TrimStart()[..1].ToUpperInvariant();
    [JsonIgnore] public string TintHex => $"#{TintColor}";
    [JsonIgnore] public string TintLabel => $"{Math.Round(TintOpacity / 255d * 100)}%";
    [JsonIgnore] public string ModeLabel => Mode == GlassMode.Native ? "Native" : "Host";
    public event PropertyChangedEventHandler? PropertyChanged;

    // Hosts run on their own threads and must not see edits that haven't been applied yet.
    public AppProfile Clone()
    {
        var copy = (AppProfile)MemberwiseClone();
        copy.PropertyChanged = null; // MemberwiseClone copies the UI's subscriptions too.
        return copy;
    }

    public void ResetAppearance()
    {
        WindowOpacity = DefaultWindowOpacity;
        TintOpacity = DefaultTintOpacity;
        TintColor = DefaultTintColor;
        ExtendIntoClient = false;
        KeepGlassWhenUnfocused = true;
        Corners = CornerStyle.Default;
        TitleBar = TitleBarTheme.Default;
    }

    private void Set<T>(ref T field, T value, [CallerMemberName] string? property = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return;
        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(property));
        if (property == nameof(WindowOpacity)) PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(OpacityLabel)));
        if (property == nameof(TintOpacity)) PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(TintLabel)));
        if (property == nameof(ProcessName)) PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(IsExplorer)));
        if (property == nameof(DisplayName)) PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Initial)));
        if (property == nameof(TintColor)) PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(TintHex)));
        if (property == nameof(Mode)) PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(ModeLabel)));
    }
}
