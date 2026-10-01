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
            // UWP apps (Settings, Calculator) show up as ApplicationFrameHost's frame; list the app inside instead.
            var content = NativeMethods.FindWindowEx(window, nint.Zero, AppProfile.UwpContentWindowClass, null);
            if (content != nint.Zero && NativeMethods.GetWindowThreadProcessId(content, out var hostedId) != 0 && hostedId != processId)
                processId = hostedId;
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
                var displayName = content != nint.Zero && !string.IsNullOrWhiteSpace(windowTitle) ? windowTitle
                    : !string.IsNullOrWhiteSpace(process.MainWindowTitle)
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

    internal static nint FindLargestWindow(string processName) => FindLargestWindow(processName, out _);

    internal static nint FindLargestWindow(string processName, out HashSet<uint> processIds)
    {
        var ids = processIds = GetProcessIds(processName);
        nint best = nint.Zero;
        long bestArea = 0;
        NativeMethods.EnumWindows((window, _) =>
        {
            if (!NativeMethods.IsWindowVisible(window)) return true;
            NativeMethods.GetWindowThreadProcessId(window, out var processId);
            if (!ids.Contains(processId)) return true;
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

    internal static HashSet<uint> GetProcessIds(string processName) =>
        Process.GetProcessesByName(processName).Select(process => (uint)process.Id).ToHashSet();

    /// <summary>
    /// True for windows that have a real frame worth styling: visible, top-level, and not a menu, tooltip,
    /// or overlay. Same eligibility rules MicaForEveryone uses (MIT).
    /// </summary>
    internal static bool IsFramedAppWindow(nint window)
    {
        if (!NativeMethods.IsWindowVisible(window) || NativeMethods.GetAncestor(window, NativeMethods.GaRoot) != window) return false;
        var style = NativeMethods.GetWindowLongPtr(window, NativeMethods.GwlStyle).ToInt64();
        var exStyle = NativeMethods.GetWindowLongPtr(window, NativeMethods.GwlExStyle).ToInt64();
        if ((exStyle & (NativeMethods.WsExNoActivate | NativeMethods.WsExTransparent)) != 0) return false;
        var hasTitleBar = (style & NativeMethods.WsBorder) != 0 && (style & NativeMethods.WsDlgFrame) != 0;
        if ((exStyle & NativeMethods.WsExToolWindow) != 0 && !hasTitleBar) return false;
        if ((style & NativeMethods.WsPopup) != 0 && !hasTitleBar) return false;
        return true;
    }

    internal static bool IsProcessNamed(uint processId, string processName)
    {
        try
        {
            using var process = Process.GetProcessById((int)processId);
            return string.Equals(process.ProcessName, processName, StringComparison.OrdinalIgnoreCase);
        }
        catch (ArgumentException) { return false; }
        catch (InvalidOperationException) { return false; }
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
