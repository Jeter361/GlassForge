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

    public static void SetEnabled(bool enabled)
    {
        using var key = Registry.CurrentUser.CreateSubKey(KeyPath, true);
        if (!enabled) { key.DeleteValue(ValueName, false); return; }
        var executable = Environment.ProcessPath ?? throw new InvalidOperationException("Unable to locate GlassForge executable.");
        key.SetValue(ValueName, $"\"{executable}\" --background");
    }
}

