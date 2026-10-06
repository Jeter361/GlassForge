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
    private bool _solidTextArea;
    private bool _dxgiGlass;
    private bool _explorerGlass;
    private string _executablePath = "";
    private System.Windows.Media.ImageSource? _icon;
    private bool _iconLoaded;

    public Guid Id { get; set; } = Guid.NewGuid();
    public string DisplayName { get => _displayName; set => Set(ref _displayName, value); }
    public string ProcessName { get => _processName; set => Set(ref _processName, Path.GetFileNameWithoutExtension(value.Trim())); }
    /// <summary>
    /// Experimental, File Explorer and Windows Terminal only: loads GlassForge's hook into the app. In Explorer it
    /// redraws backgrounds, scrollbars and text for glass; in Terminal it only stops the app resetting its backdrop
    /// and frame, so the glass reaches the tab row without flashing. The only GlassForge feature that injects code;
    /// off unless the user opts in. Saved as "ExplorerGlass", its name when it was Explorer-only.
    /// </summary>
    [JsonPropertyName("ExplorerGlass")]
    public bool HookGlass { get => _explorerGlass; set => Set(ref _explorerGlass, value); }
    /// <summary>
    /// Apps that draw their own title bar inside the window. The native backdrop sits behind the frame, which their
    /// content covers, so it only shows once extended into the whole window.
    /// </summary>
    private static readonly HashSet<string> OwnTitleBarApps = new(StringComparer.OrdinalIgnoreCase) { "notepad" };
    /// <summary>Apps the opt-in hook makes glass (WINWORD, EXCEL, POWERPNT, ms-teams). Office apps get their own
    /// Mica brush swapped; Teams gets its native chrome shown live via the host-backdrop opt-in (its WebView2 chat
    /// pane stays opaque).</summary>
    private static readonly HashSet<string> OfficeApps = new(StringComparer.OrdinalIgnoreCase) { "WINWORD", "EXCEL", "POWERPNT", "ms-teams" };
    [JsonIgnore] public bool IsNotepad => string.Equals(ProcessName, "notepad", StringComparison.OrdinalIgnoreCase);
    [JsonIgnore] public bool DrawsOwnTitleBar => OwnTitleBarApps.Contains(ProcessName);
    /// <summary>Last known full path of the app's exe, for its icon while the app isn't running.</summary>
    public string ExecutablePath { get => _executablePath; set => Set(ref _executablePath, value ?? ""); }

    /// <summary>The app's own icon, or null to show the letter tile. Looked up once, on first display.</summary>
    [JsonIgnore]
    public System.Windows.Media.ImageSource? Icon
    {
        get
        {
            if (_iconLoaded) return _icon;
            _iconLoaded = true;
            if (!System.IO.File.Exists(ExecutablePath) && Services.AppIconService.FindExecutable(ProcessName) is { } found)
                ExecutablePath = found;
            return _icon = Services.AppIconService.For(ExecutablePath);
        }
    }
    [JsonIgnore] public bool HasIcon => Icon is not null;
    [JsonIgnore] public bool IsExplorer => string.Equals(ProcessName, "explorer", StringComparison.OrdinalIgnoreCase);
    [JsonIgnore] public bool IsSettings => string.Equals(ProcessName, "SystemSettings", StringComparison.OrdinalIgnoreCase);
    [JsonIgnore] public bool IsTerminal => string.Equals(ProcessName, "WindowsTerminal", StringComparison.OrdinalIgnoreCase);
    /// <summary>Word, Excel or PowerPoint: they all draw their own Mica, so the same hook swap applies (office.cpp).</summary>
    [JsonIgnore] public bool IsOffice => OfficeApps.Contains(ProcessName);
    /// <summary>The Office app's friendly name for the toggle's warning text.</summary>
    [JsonIgnore] public string OfficeDisplayName => ProcessName.ToUpperInvariant() switch
    {
        "WINWORD" => "Word",
        "EXCEL" => "Excel",
        "POWERPNT" => "PowerPoint",
        "MS-TEAMS" => "Teams",
        _ => "Office"
    };
    /// <summary>The Office app's executable name for the toggle's warning text.</summary>
    [JsonIgnore] public string OfficeExeName => ProcessName.ToUpperInvariant() + ".EXE";
    /// <summary>The top-level window class the hook styles in this app, or null when the app has no hook mode.</summary>
    [JsonIgnore] public string? HookWindowClass => IsExplorer ? "CabinetWClass" : IsTerminal ? "CASCADIA_HOSTING_WINDOW_CLASS" : IsSettings ? UwpContentWindowClass : IsOffice ? AnyWindowClass : null;
    /// <summary>The app has an experimental hook mode, so the editor offers its toggle.</summary>
    [JsonIgnore] public bool HasHook => HookWindowClass is not null;
    /// <summary>What the hook does for this app, in one or two sentences.</summary>
    [JsonIgnore] public string HookSummary =>
        IsExplorer ? "Glass behind the file list, navigation pane, scrollbars and toolbar, not just the title bar."
        : IsTerminal ? "Live glass behind the tab row only; needs a Terminal theme with a transparent tab row."
        : IsSettings ? "Swaps Settings' static Mica for a live blur, animated wallpapers included."
        : string.Equals(ProcessName, "ms-teams", StringComparison.OrdinalIgnoreCase) ? "Live glass on Teams' title bar and frame; the chat pane stays solid."
        : $"Swaps {OfficeDisplayName}'s static Mica for a live blur. The first click on a window may only activate it.";
    /// <summary>The red warning under the toggle: this is the one feature that injects code.</summary>
    [JsonIgnore] public string HookWarning =>
        IsExplorer ? "Injects code into explorer.exe, which also runs your taskbar and desktop: a bug can crash them (Windows restarts Explorer). Antivirus may flag it."
        : IsTerminal ? "Injects code into Windows Terminal. All tabs share one process, so a crash closes every tab. Antivirus may flag it."
        : IsSettings ? "Injects code into Settings. A crash only closes Settings. Antivirus may flag it."
        : $"Injects code into {OfficeDisplayName}. Save your work first: a crash closes every {OfficeDisplayName} window. Fully turning it off needs a restart of the app.";
    /// <summary>A UWP app's UI thread owns this window, hosted inside ApplicationFrameHost's frame.</summary>
    public const string UwpContentWindowClass = "Windows.UI.Core.CoreWindow";
    /// <summary>Every top-level window of the app's process: Word makes its backdrop at startup, so the hook has to
    /// reach it through the splash screen, before the main window exists.</summary>
    public const string AnyWindowClass = "*";
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
    /// <summary>
    /// Notepad with the frame extended: keep the text area solid and put the glass only around it. Needed with HDR on,
    /// where Windows composes the see-through text area straight instead of additively, so the text vanishes with it.
    /// </summary>
    public bool SolidTextArea { get => _solidTextArea; set => Set(ref _solidTextArea, value); }
    /// <summary>
    /// Experimental, Native mode: glass inside apps that draw every pixel with Direct3D (Qt, Chromium/Electron),
    /// by turning their background color transparent on every frame (DxgiGlassSession). Injects code and restarts the
    /// app when turned on or off.
    /// </summary>
    public bool DxgiGlass { get => _dxgiGlass; set => Set(ref _dxgiGlass, value); }

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
        ExtendIntoClient = DrawsOwnTitleBar;
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
        if (property == nameof(ProcessName))
        {
            _iconLoaded = false;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Icon)));
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(HasIcon)));
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(IsExplorer)));
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(IsTerminal)));
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(IsSettings)));
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(IsOffice)));
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(IsNotepad)));
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(HasHook)));
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(HookSummary)));
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(HookWarning)));
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(OfficeDisplayName)));
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(OfficeExeName)));
        }
        if (property == nameof(DisplayName)) PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Initial)));
        if (property == nameof(TintColor)) PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(TintHex)));
        if (property == nameof(Mode)) PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(ModeLabel)));
    }
}
