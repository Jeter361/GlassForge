using System.Diagnostics;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Windows.Automation;
using GlassForge.Models;
using GlassForge.Native;

namespace GlassForge.Services;

/// <summary>
/// Experimental glass for apps that draw every pixel with Direct3D (Qt, Chromium/Electron, Flutter): the hook
/// (native/hook/dxgiglass.cpp) turns the app's background color into transparency on every frame, over the live
/// Acrylic the native session sets. The hook has to be in the app before it creates its swap chain, so the app runs
/// started through the hook (rundll32 ...,LaunchWithGlass): turning this on restarts the app once, and an app started
/// normally while it is on is restarted the same way. Areas that must stay solid (LogMeIn's remote screen) are found
/// with UI Automation and handed to the hook. See docs/GLASS_WRITEUP.md, "Update 9".
/// </summary>
internal sealed class DxgiGlassSession(AppProfile profile) : IGlassSession
{
    private readonly AppProfile _profile = profile.Clone();
    private readonly CancellationTokenSource _stop = new();
    private Thread? _thread;

    public void Start()
    {
        DxgiGlassList.Set(_profile.ProcessName, true);
        _thread = new Thread(Run) { IsBackground = true, Name = $"GlassForge dxgi:{_profile.ProcessName}" };
        _thread.SetApartmentState(ApartmentState.MTA); // UI Automation clients belong off STA threads
        _thread.Start();
    }

    // Quitting GlassForge leaves a running app as it is (the hook keeps working on its own); only turning the option
    // off restarts the app without the hook (RelaunchWithout).
    public void Dispose()
    {
        _stop.Cancel();
        _thread?.Join(3000);
        _stop.Dispose();
    }

    private void Run()
    {
        NativeMethods.SetThreadDpiAwarenessContext(NativeMethods.DpiAwarenessContextPerMonitorAwareV2);
        var keep = KeepSolidRule.For(_profile.ProcessName);
        KeepAreaPublisher? publisher = null;
        var lastCheck = DateTime.MinValue;
        try
        {
            while (!_stop.IsCancellationRequested)
            {
                if ((DateTime.Now - lastCheck).TotalSeconds >= 2)
                {
                    lastCheck = DateTime.Now;
                    var target = MainProcess(_profile.ProcessName);
                    if (target is not null && !HasHook(target))
                    {
                        Log($"{_profile.ProcessName} is running without the hook; restarting it with glass");
                        Relaunch(target, withGlass: true);
                        target = null;
                    }
                    if (target is not null && publisher?.ProcessId != target.Id)
                    {
                        publisher?.Dispose();
                        publisher = keep is null ? null : new KeepAreaPublisher(target.Id, target.MainWindowHandle, keep);
                    }
                    target?.Dispose();
                }
                publisher?.Update();
                _stop.Token.WaitHandle.WaitOne(500);
            }
        }
        catch (Exception exception) { Log($"stopped: {exception.Message}"); }
        finally { publisher?.Dispose(); }
    }

    /// <summary>Called when the option is turned off: the hook can't leave a running app, so the app restarts plainly.</summary>
    internal static void RelaunchWithout(string processName)
    {
        DxgiGlassList.Set(processName, false);
        using var target = MainProcess(processName);
        if (target is not null && HasHook(target)) Relaunch(target, withGlass: false);
    }

    private static Process? MainProcess(string processName)
    {
        foreach (var process in Process.GetProcessesByName(processName))
        {
            if (process.MainWindowHandle != nint.Zero) return process;
            process.Dispose();
        }
        return null;
    }

    private static bool HasHook(Process process)
    {
        try
        {
            foreach (ProcessModule module in process.Modules)
                if (module.ModuleName.StartsWith("GlassForge.Hook", StringComparison.OrdinalIgnoreCase)) return true;
        }
        catch (Exception exception) when (exception is System.ComponentModel.Win32Exception or InvalidOperationException) { }
        return false;
    }

    private static void Relaunch(Process process, bool withGlass)
    {
        string? exe;
        try { exe = process.MainModule?.FileName; }
        catch (System.ComponentModel.Win32Exception) { exe = null; }
        if (exe is null) return;
        var dll = withGlass ? HookLibrary.Extract() : null;
        if (withGlass && dll is null) { Log("the hook DLL isn't available in this build"); return; }
        process.CloseMainWindow();
        if (!process.WaitForExit(5000))
        {
            try { process.Kill(); process.WaitForExit(3000); }
            catch (InvalidOperationException) { }
        }
        var start = withGlass
            ? new ProcessStartInfo("rundll32.exe", $"\"{dll}\",LaunchWithGlass \"{exe}\"") { UseShellExecute = false }
            : new ProcessStartInfo(exe) { UseShellExecute = true };
        Process.Start(start)?.Dispose();
        Log($"restarted {Path.GetFileName(exe)} {(withGlass ? "with" : "without")} glass");
    }

    internal static void Log(string message) => Debug.WriteLine($"[dxgi glass] {message}");
}

/// <summary>%LOCALAPPDATA%\GlassForge\dxgi.txt: the exe names the hook turns its DXGI glass on in, read at launch.</summary>
internal static class DxgiGlassList
{
    private static readonly object Lock = new();
    private static string FilePath => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "GlassForge", "dxgi.txt");

    internal static void Set(string processName, bool on)
    {
        var exe = processName + ".exe";
        lock (Lock)
        {
            var names = File.Exists(FilePath)
                ? File.ReadAllLines(FilePath).Where(line => line.Length > 0).ToList()
                : [];
            names.RemoveAll(name => string.Equals(name, exe, StringComparison.OrdinalIgnoreCase));
            if (on) names.Add(exe);
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
            File.WriteAllLines(FilePath, names);
        }
    }
}

/// <summary>How to find an app's must-stay-solid area with UI Automation.</summary>
internal sealed record KeepSolidRule(string DocumentPrefix, string AutomationIdSuffix)
{
    // LogMeIn Resolve: the remote screen is the session page's <video> element.
    internal static KeepSolidRule? For(string processName) =>
        string.Equals(processName, "ra-technician-console", StringComparison.OrdinalIgnoreCase)
            ? new KeepSolidRule("session", "VideoStream_Video")
            : null;
}

/// <summary>
/// Publishes the keep-solid rectangles (client pixels) in Local\GlassForge.DxgiKeep.&lt;pid&gt;, where the hook reads
/// them. LogMeIn's embedded Chromium stops reporting the inside of the session page after full screen is toggled, so
/// while the video element is visible its insets inside the page are remembered, and applied to the page later.
/// Found elements are reused; a full search (many calls into the app) only every 3 s or when something is lost.
/// </summary>
internal sealed class KeepAreaPublisher : IDisposable
{
    private readonly nint _window;
    private readonly KeepSolidRule _rule;
    private readonly MemoryMappedFile _mapping;
    private readonly MemoryMappedViewAccessor _view;
    private AutomationElement? _page;
    private AutomationElement? _video;
    private DateTime _searched = DateTime.MinValue;
    private double[]? _insets;
    private string _lastFrame = "";

    internal int ProcessId { get; }

    internal KeepAreaPublisher(int processId, nint window, KeepSolidRule rule)
    {
        ProcessId = processId;
        _window = window;
        _rule = rule;
        _mapping = MemoryMappedFile.CreateOrOpen($"Local\\GlassForge.DxgiKeep.{processId}", 4 + 8 * 16);
        _view = _mapping.CreateViewAccessor();
    }

    internal void Update()
    {
        // While the window moves or resizes, the two coordinate readings disagree; keep what was published until it settles.
        if (!NativeMethods.GetWindowRect(_window, out var frame)) return;
        var frameText = $"{frame.Left},{frame.Top},{frame.Right},{frame.Bottom}";
        if (frameText != _lastFrame) { _lastFrame = frameText; _searched = DateTime.MinValue; return; }
        int[]? area = null;
        try { area = Measure(); }
        catch (ElementNotAvailableException) { _page = null; _video = null; }
        catch (System.Runtime.InteropServices.COMException) { _page = null; _video = null; }
        if (area is null) { _view.Write(0, 0); return; }
        for (var i = 0; i < 4; i++) _view.Write(4 + i * 4, area[i]);
        _view.Write(0, 1);
    }

    private int[]? Measure()
    {
        if (_page is null || (DateTime.Now - _searched).TotalSeconds >= 3)
        {
            _searched = DateTime.Now;
            _page = FindPage();
            _video = _page is null ? null : FindVideo(_page, 0);
        }
        if (_page is null) return null;
        var p = _page.Current.BoundingRectangle;
        if (p.IsEmpty) return null;
        System.Windows.Rect? v = null;
        if (_video is not null)
        {
            try { v = _video.Current.BoundingRectangle; }
            catch (ElementNotAvailableException) { _video = null; }
        }
        NativeMethods.GetClientRect(_window, out var client);
        var origin = new NativeMethods.Point();
        NativeMethods.ClientToScreen(_window, ref origin);
        // A real video rectangle is non-empty, inside the window and inside its page (after full screen ends, the old
        // element lingers for seconds with its full-screen size).
        System.Windows.Rect box;
        if (v is { IsEmpty: false } r && r.Width >= 1 && r.Width <= client.Right + 8 && r.Height <= client.Bottom + 8
            && r.Left >= p.Left - 2 && r.Top >= p.Top - 2 && r.Right <= p.Right + 2 && r.Bottom <= p.Bottom + 2)
        {
            _insets = [r.Left - p.Left, r.Top - p.Top, p.Right - r.Right, p.Bottom - r.Bottom];
            box = r;
        }
        else if (_insets is { } i)
            box = new System.Windows.Rect(new System.Windows.Point(p.Left + i[0], p.Top + i[1]), new System.Windows.Point(p.Right - i[2], p.Bottom - i[3]));
        else box = p; // nothing better known yet: the whole page stays solid
        return [(int)(box.Left - origin.X), (int)(box.Top - origin.Y), (int)(box.Right - origin.X), (int)(box.Bottom - origin.Y)];
    }

    private AutomationElement? FindPage()
    {
        var documents = new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Document);
        foreach (AutomationElement document in AutomationElement.FromHandle(_window).FindAll(TreeScope.Descendants, documents))
            if (document.Current.Name.StartsWith(_rule.DocumentPrefix, StringComparison.Ordinal)) return document;
        return null;
    }

    // The video element only shows up in the raw view.
    private AutomationElement? FindVideo(AutomationElement element, int depth)
    {
        if (depth > 40) return null;
        for (var child = TreeWalker.RawViewWalker.GetFirstChild(element); child is not null; child = TreeWalker.RawViewWalker.GetNextSibling(child))
        {
            if (child.Current.AutomationId.EndsWith(_rule.AutomationIdSuffix, StringComparison.Ordinal)) return child;
            if (FindVideo(child, depth + 1) is { } found) return found;
        }
        return null;
    }

    public void Dispose()
    {
        _view.Dispose();
        _mapping.Dispose();
    }
}
