namespace GlassForge.Models;

public sealed record RunningApplication(string DisplayName, string ProcessName, int ProcessId, string WindowTitle)
{
    // No cached fields here: a record's generated equality covers every field, so the icon is cached by path in
    // AppIconService instead.
    public string? ExecutablePath => GlassForge.Services.AppIconService.ExecutablePath(ProcessId);

    /// <summary>The app's own icon, or null to show the letter tile.</summary>
    public System.Windows.Media.ImageSource? Icon =>
        string.Equals(ProcessName, "ApplicationFrameHost", StringComparison.OrdinalIgnoreCase)
            ? GlassForge.Services.AppIconService.ForHostedApp(ProcessId, WindowTitle)
            : GlassForge.Services.AppIconService.For(ExecutablePath, ProcessId);

    public bool HasIcon => Icon is not null;
    /// <summary>The app's own name rather than its current window title.</summary>
    public string Name => GlassForge.Services.AppIconService.FriendlyName(ExecutablePath) ?? DisplayName;
    public string Initial => string.IsNullOrWhiteSpace(DisplayName) ? "?" : DisplayName.TrimStart()[..1].ToUpperInvariant();

    public string Summary => string.IsNullOrWhiteSpace(WindowTitle)
        ? $"{ProcessName}.exe · PID {ProcessId}"
        : $"{WindowTitle} · {ProcessName}.exe";
}
