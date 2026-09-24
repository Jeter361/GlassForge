using Microsoft.Win32;

namespace GlassForge.Services;

public static class StartupService
{
    private const string ValueName = "GlassForge";
    private const string KeyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";

    public static bool IsEnabled
    {
        get
        {
            using var key = Registry.CurrentUser.OpenSubKey(KeyPath, false);
            return key?.GetValue(ValueName) is string;
        }
    }

    /// <summary>Points an existing startup entry at this executable, so an older copy doesn't come back at sign-in.</summary>
    public static void RepointIfStale()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(KeyPath, false);
            if (key?.GetValue(ValueName) is not string command || Environment.ProcessPath is not { } executable) return;
            if (!command.Contains(executable, StringComparison.OrdinalIgnoreCase)) SetEnabled(true);
        }
        catch (System.Security.SecurityException) { }
        catch (UnauthorizedAccessException) { }
    }

    public static void SetEnabled(bool enabled)
    {
        using var key = Registry.CurrentUser.CreateSubKey(KeyPath, true);
        if (!enabled) { key.DeleteValue(ValueName, false); return; }
        var executable = Environment.ProcessPath ?? throw new InvalidOperationException("Unable to locate GlassForge executable.");
        key.SetValue(ValueName, $"\"{executable}\" --background");
    }
}

