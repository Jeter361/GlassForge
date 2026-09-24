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
            if (!NativeMethods.GetWindowRect(window, out var rect)) return true;
            if (rect.Right <= rect.Left || rect.Bottom <= rect.Top) return true;

            try
            {
                using var process = Process.GetProcessById((int)processId);
                var name = process.ProcessName;
                if (string.Equals(name, "GlassForge", StringComparison.OrdinalIgnoreCase)) return true;
                var windowTitle = title.ToString();
                var displayName = !string.IsNullOrWhiteSpace(process.MainWindowTitle)
                    ? process.MainWindowTitle
                    : !string.IsNullOrWhiteSpace(windowTitle) ? windowTitle : name;
                results.TryAdd(name, new RunningApplication(
                    displayName,
                    name,
                    process.Id,
                    windowTitle));
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
            if (!IsUserVisible(window)) return true;
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

    // IsWindowVisible is true for cloaked windows (other virtual desktops, suspended UWP and Chromium frames)
    // and for tool windows; backing either leaves a glass panel with nothing in front of it.
    private static bool IsUserVisible(nint window)
    {
        if ((NativeMethods.GetWindowLongPtr(window, NativeMethods.GwlExStyle).ToInt64() & NativeMethods.WsExToolWindow) != 0) return false;
        return !IsCloaked(window);
    }

    internal static bool IsCloaked(nint window) =>
        NativeMethods.DwmGetWindowAttribute(window, NativeMethods.DwmwaCloaked, out int cloaked, sizeof(int)) == 0 && cloaked != 0;
}
