using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using GlassForge.Models;
using GlassForge.Native;

namespace GlassForge.Services;

/// <summary>The way GlassForge can give an app glass, best first.</summary>
public enum GlassRoute
{
    /// <summary>The app has its own tested hook mode (Explorer, Terminal, Settings, Office, Teams).</summary>
    LiveGlassHook,
    /// <summary>Native Acrylic over the whole window: the app leaves its background clear (Mica/XAML apps, Notepad).</summary>
    NativeWholeWindow,
    /// <summary>Native Acrylic on the frame only: the app paints its own background but has a system title bar.</summary>
    NativeFrame,
    /// <summary>The app draws with Direct3D and paints every pixel: only the experimental DXGI glass reaches inside.</summary>
    ExperimentalDxgi,
    /// <summary>A glass layer behind a translucent window: works for any app GlassForge can reach.</summary>
    Overlay,
    /// <summary>GlassForge can't reach the app at all (it runs as administrator).</summary>
    Unreachable
}

/// <summary>What a read-only look at a running app found, and the route that fits it.</summary>
public sealed record AppDiagnosis(
    string Framework,
    string Rendering,
    bool Packaged,
    bool Elevated,
    bool OwnTitleBar,
    string Backdrop,
    IReadOnlyList<string> Notes,
    GlassRoute Route,
    string Reason)
{
    public string RouteLabel => Route switch
    {
        GlassRoute.LiveGlassHook => "Live glass (this app's tested hook)",
        GlassRoute.NativeWholeWindow => "Native, whole window",
        GlassRoute.NativeFrame => "Native, frame only",
        GlassRoute.ExperimentalDxgi => "Experimental DXGI glass",
        GlassRoute.Overlay => "Overlay",
        _ => "Not reachable"
    };
}

/// <summary>
/// Looks at a running app without touching it (no injection, nothing written): which UI framework it is built on,
/// whether it draws with GDI or Direct3D, whether it is a Store package or runs as administrator, and what its window
/// does with the DWM backdrop. Each of those decided a route by hand before (see docs/GLASS_WRITEUP.md); this makes the
/// same call automatically.
/// </summary>
internal static class AppDiagnosisService
{
    // Module name (lower case) -> framework, checked in this order: the more specific stacks first.
    private static readonly (string Module, string Framework)[] Frameworks =
    [
        ("mso40uiwin32client.dll", "Microsoft Office"),
        ("embeddedbrowserwebview.dll", "WebView2 (Chromium)"),
        ("libcef.dll", "CEF (Chromium)"),
        ("flutter_windows.dll", "Flutter"),
        ("qt6webenginecore.dll", "Qt 6 + QtWebEngine (Chromium)"),
        ("qt5webenginecore.dll", "Qt 5 + QtWebEngine (Chromium)"),
        ("qt6core.dll", "Qt 6"),
        ("qt5core.dll", "Qt 5"),
        ("microsoft.ui.xaml.dll", "WinUI 3"),
        ("wpfgfx_cor3.dll", "WPF"),
        ("wpfgfx_v0400.dll", "WPF"),
        ("system.windows.forms.dll", "WinForms"),
        ("system.windows.forms.ni.dll", "WinForms"),
        ("windows.ui.xaml.dll", "System XAML (UWP)"),
        ("jvm.dll", "Java"),
    ];

    private const long WsExNoRedirectionBitmap = 0x00200000;
    private const long WsCaption = 0x00C00000;

    internal static AppDiagnosis Diagnose(AppProfile profile)
    {
        var notes = new List<string>();
        var window = WindowDiscoveryService.FindLargestWindow(profile.ProcessName, out var processIds);
        if (window == nint.Zero || processIds.Count == 0)
            return new AppDiagnosis("Unknown", "Unknown", false, false, false, "Unknown",
                ["The app isn't running, or has no visible window. Open it and diagnose again."], GlassRoute.Overlay,
                "Nothing to look at yet; Overlay works for most apps.");
        NativeMethods.GetWindowThreadProcessId(window, out var processId);

        var elevated = IsElevated((int)processId);
        var packaged = IsPackaged((int)processId, AppIconService.ExecutablePath((int)processId));
        var modules = LoadedModules((int)processId);
        var className = ClassOf(window);
        var children = ChildClasses(window);
        var framework = DetectFramework(modules, className, children, AppIconService.ExecutablePath((int)processId));

        var style = NativeMethods.GetWindowLongPtr(window, NativeMethods.GwlStyle).ToInt64();
        var exStyle = NativeMethods.GetWindowLongPtr(window, NativeMethods.GwlExStyle).ToInt64();
        var ownTitleBar = (style & WsCaption) != WsCaption;
        var noRedirection = (exStyle & WsExNoRedirectionBitmap) != 0;
        var rendering = noRedirection ? "DirectComposition (no GDI surface)" : DetectRendering(window);
        var backdrop = BackdropOf(window);

        if (elevated) notes.Add("Runs as administrator: Windows blocks a normal GlassForge from styling it.");
        if (packaged) notes.Add("Store/MSIX package: it can't be relaunched with the DXGI hook.");
        if (ownTitleBar) notes.Add("Draws its own title bar, so frame-only glass has nothing to show through.");
        if (children.Contains("Intermediate D3D Window")) notes.Add("Chromium draws it in a separate GPU process.");
        if (modules.Contains("windhawk.dll")) notes.Add("Windhawk is loaded into it; its mods may fight GlassForge.");
        if (modules.Count == 0 && !elevated) notes.Add("Its loaded DLLs couldn't be read; the framework is a guess.");

        var (route, reason) = Decide(profile, framework, rendering, packaged, elevated, ownTitleBar, backdrop, noRedirection);
        return new AppDiagnosis(framework, rendering, packaged, elevated, ownTitleBar, backdrop, notes, route, reason);
    }

    internal static void WriteReport(IEnumerable<string> processNames)
    {
        var report = new StringBuilder();
        foreach (var name in processNames)
        {
            var profile = new AppProfile { ProcessName = name };
            var d = Diagnose(profile);
            report.AppendLine($"== {name}")
                .AppendLine($"   framework: {d.Framework}; rendering: {d.Rendering}; backdrop: {d.Backdrop}")
                .AppendLine($"   packaged: {d.Packaged}; admin: {d.Elevated}; own title bar: {d.OwnTitleBar}")
                .AppendLine($"   route: {d.RouteLabel} - {d.Reason}");
            foreach (var note in d.Notes) report.AppendLine($"   note: {note}");
        }
        var folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "GlassForge");
        Directory.CreateDirectory(folder);
        File.WriteAllText(Path.Combine(folder, "diagnosis.txt"), report.ToString());
    }

    // The decision table, from what each hand-tuned app taught (writeup Updates 1-9).
    private static (GlassRoute, string) Decide(AppProfile profile, string framework, string rendering, bool packaged, bool elevated,
        bool ownTitleBar, string backdrop, bool noRedirection)
    {
        if (elevated)
            return (GlassRoute.Unreachable, "It runs as administrator; run GlassForge as administrator to style it.");
        if (profile.HasHook)
            return (GlassRoute.LiveGlassHook, "GlassForge has a tested live-glass hook for this app.");
        var direct3D = rendering.StartsWith("Direct3D", StringComparison.Ordinal) || noRedirection;
        var chromium = framework.Contains("Chromium", StringComparison.Ordinal) || framework.Contains("Electron", StringComparison.Ordinal);
        if (profile.DrawsOwnTitleBar)
            return (GlassRoute.NativeWholeWindow, "It leaves its background clear behind its own title bar (like Notepad).");
        // Acrylic on a window GlassForge already styles natively is GlassForge's own, not the app's choice.
        var ownBackdrop = backdrop is "Mica" or "Mica Alt" || (backdrop == "Acrylic" && !(profile.Enabled && profile.Mode == GlassMode.Native));
        if (ownBackdrop && !chromium)
            return (GlassRoute.NativeWholeWindow, "It already asks Windows for a backdrop and leaves its background clear, so live Acrylic can replace it.");
        if (direct3D && !packaged && !framework.StartsWith("WebView2", StringComparison.Ordinal))
            return (GlassRoute.ExperimentalDxgi, $"{framework} paints every pixel with Direct3D; only the experimental DXGI glass reaches inside (tested on Qt). Needs an app restart.");
        if (direct3D || chromium)
            return (GlassRoute.Overlay, packaged
                ? "It paints every pixel and is a Store package, so neither Native nor the DXGI hook reach inside; Overlay makes the whole window translucent."
                : "It paints every pixel; Overlay makes the whole window translucent over glass.");
        if (!ownTitleBar && framework is "Win32 (GDI)" or "Microsoft Office")
            return (GlassRoute.NativeFrame, "A classic GDI app with a system title bar: Native glass shows on the frame; use Overlay for the whole window.");
        return (GlassRoute.Overlay, "It paints its own background; Overlay works for any app.");
    }

    private static string DetectFramework(HashSet<string> modules, string className, HashSet<string> children, string? exePath)
    {
        foreach (var (module, framework) in Frameworks)
            if (modules.Contains(module)) return framework;
        if (className.StartsWith("Chrome_WidgetWin", StringComparison.Ordinal) || children.Contains("Intermediate D3D Window"))
            return exePath is not null && File.Exists(Path.Combine(Path.GetDirectoryName(exePath) ?? "", "resources", "app.asar"))
                ? "Electron (Chromium)" : "Chromium";
        if (className == "ApplicationFrameWindow" || children.Contains("Windows.UI.Core.CoreWindow")) return "System XAML (UWP)";
        return "Win32 (GDI)";
    }

    // A window drawn by a flip-model swap chain comes back black from PrintWindow without PW_RENDERFULLCONTENT, while
    // the full-content capture shows its pixels; a GDI window looks the same both ways.
    private static string DetectRendering(nint window)
    {
        if (!NativeMethods.GetWindowRect(window, out var rect)) return "Unknown";
        var width = Math.Min(rect.Right - rect.Left, 1600);
        var height = Math.Min(rect.Bottom - rect.Top, 1200);
        if (width < 32 || height < 32 || NativeMethods.IsIconic(window)) return "Unknown (minimized)";
        var plain = DarkShare(window, width, height, 0);
        var full = DarkShare(window, width, height, PwRenderFullContent);
        if (plain < 0 || full < 0) return "Unknown";
        return plain > 0.95 && full < 0.8 ? "Direct3D (swap chain)" : "GDI";
    }

    private static double DarkShare(nint window, int width, int height, uint flags)
    {
        using var bitmap = new System.Drawing.Bitmap(width, height);
        using (var graphics = System.Drawing.Graphics.FromImage(bitmap))
        {
            var dc = graphics.GetHdc();
            try { if (!PrintWindow(window, dc, flags)) return -1; }
            finally { graphics.ReleaseHdc(dc); }
        }
        int dark = 0, total = 0;
        for (var y = height / 8; y < height; y += Math.Max(1, height / 24))
            for (var x = width / 16; x < width; x += Math.Max(1, width / 24))
            {
                var c = bitmap.GetPixel(x, y);
                if (c.R + c.G + c.B < 6) dark++;
                total++;
            }
        return total == 0 ? -1 : (double)dark / total;
    }

    private static string BackdropOf(nint window)
    {
        if (NativeMethods.DwmGetWindowAttribute(window, NativeMethods.DwmwaSystemBackdropType, out int backdrop, sizeof(int)) != 0) return "Unknown";
        return backdrop switch { 0 => "Default", 1 => "None", 2 => "Mica", 3 => "Acrylic", 4 => "Mica Alt", _ => $"Type {backdrop}" };
    }

    private static HashSet<string> LoadedModules(int processId)
    {
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        try
        {
            using var process = Process.GetProcessById(processId);
            foreach (ProcessModule module in process.Modules) names.Add(module.ModuleName.ToLowerInvariant());
        }
        catch (Exception exception) when (exception is System.ComponentModel.Win32Exception or InvalidOperationException or ArgumentException) { }
        return names;
    }

    private static string ClassOf(nint window)
    {
        var name = new StringBuilder(256);
        return NativeMethods.GetClassName(window, name, name.Capacity) > 0 ? name.ToString() : "";
    }

    private static HashSet<string> ChildClasses(nint window)
    {
        var classes = new HashSet<string>(StringComparer.Ordinal);
        NativeMethods.EnumChildWindows(window, (child, _) => { classes.Add(ClassOf(child)); return classes.Count < 200; }, nint.Zero);
        return classes;
    }

    private static bool IsPackaged(int processId, string? exePath)
    {
        if (exePath?.Contains(@"\WindowsApps\", StringComparison.OrdinalIgnoreCase) == true) return true;
        var process = OpenProcess(ProcessQueryLimitedInformation, false, processId);
        if (process == nint.Zero) return false;
        try
        {
            var length = 0;
            return GetPackageFullName(process, ref length, null) != AppModelErrorNoPackage;
        }
        finally { CloseHandle(process); }
    }

    private static bool IsElevated(int processId)
    {
        var process = OpenProcess(ProcessQueryLimitedInformation, false, processId);
        if (process == nint.Zero) return true; // a normal user can always open a normal process like this
        try
        {
            // An elevated process's token can't be opened from a normal one, which answers the question by itself.
            if (!OpenProcessToken(process, TokenQuery, out var token)) return true;
            try { return GetTokenInformation(token, TokenElevation, out var elevation, sizeof(int), out _) && elevation != 0; }
            finally { CloseHandle(token); }
        }
        finally { CloseHandle(process); }
    }

    private const uint ProcessQueryLimitedInformation = 0x1000;
    private const uint TokenQuery = 0x0008;
    private const int TokenElevation = 20;
    private const int AppModelErrorNoPackage = 15700;
    private const uint PwRenderFullContent = 2;
    [DllImport("kernel32.dll")] private static extern nint OpenProcess(uint access, bool inherit, int processId);
    [DllImport("kernel32.dll")] private static extern bool CloseHandle(nint handle);
    [DllImport("advapi32.dll")] private static extern bool OpenProcessToken(nint process, uint access, out nint token);
    [DllImport("advapi32.dll")] private static extern bool GetTokenInformation(nint token, int type, out int value, int length, out int returned);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern int GetPackageFullName(nint process, ref int length, StringBuilder? name);
    [DllImport("user32.dll")] private static extern bool PrintWindow(nint window, nint dc, uint flags);
}
