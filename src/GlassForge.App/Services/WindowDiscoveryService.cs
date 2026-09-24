using System.Diagnostics;
using System.Text;
using GlassForge.Models;
using GlassForge.Native;

namespace GlassForge.Services;

public sealed class WindowDiscoveryService
{
    public IReadOnlyList<RunningApplication> GetRunningApplications()
    {
        var results = new Dictionary<string, RunningApplication>(StringComparer.OrdinalIgnoreCase);
        NativeMethods.EnumWindows((window, _) =>
        {
            if (!NativeMethods.IsWindowVisible(window)) return true;
            NativeMethods.GetWindowThreadProcessId(window, out var processId);
            var title = new StringBuilder(512);
            NativeMethods.GetWindowText(window, title, title.Capacity);
            if (title.Length == 0) return true;

            try
            {
                using var process = Process.GetProcessById((int)processId);
                var name = process.ProcessName;
                if (string.Equals(name, "GlassForge", StringComparison.OrdinalIgnoreCase)) return true;
                results.TryAdd(name, new RunningApplication(
                    string.IsNullOrWhiteSpace(process.MainWindowTitle) ? name : process.MainWindowTitle,
                    name,
                    process.Id,
                    title.ToString()));
            }
            catch (ArgumentException) { }
            catch (InvalidOperationException) { }
            return true;
        }, nint.Zero);

        return results.Values.OrderBy(app => app.DisplayName).ToArray();
    }

    internal static nint FindLargestWindow(string processName)
    {
        var processIds = Process.GetProcessesByName(processName).Select(process => (uint)process.Id).ToHashSet();
        nint best = nint.Zero;
        long bestArea = 0;
        NativeMethods.EnumWindows((window, _) =>
        {
            if (!NativeMethods.IsWindowVisible(window)) return true;
            NativeMethods.GetWindowThreadProcessId(window, out var processId);
            if (!processIds.Contains(processId)) return true;
            if (!NativeMethods.GetWindowRect(window, out var rect)) return true;
            var area = (long)(rect.Right - rect.Left) * (rect.Bottom - rect.Top);
            if (area > bestArea)
            {
                bestArea = area;
                best = window;
            }
            return true;
        }, nint.Zero);
        return best;
    }
}

