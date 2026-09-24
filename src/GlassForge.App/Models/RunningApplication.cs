namespace GlassForge.Models;

public sealed record RunningApplication(string DisplayName, string ProcessName, int ProcessId, string WindowTitle)
{
    public string Summary => string.IsNullOrWhiteSpace(WindowTitle)
        ? $"{ProcessName}.exe · PID {ProcessId}"
        : $"{WindowTitle} · {ProcessName}.exe";
}
