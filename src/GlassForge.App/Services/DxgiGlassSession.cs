using System.Diagnostics;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Automation;
using GlassForge.Models;
using GlassForge.Native;

namespace GlassForge.Services;

/// <summary>
/// Experimental glass for apps that draw every pixel with Direct3D (Qt, Chromium/Electron, Flutter): the hook
/// (native/hook/dxgiglass.cpp) turns the app's background color into transparency on every frame, over the live
/// Acrylic the native session sets. The hook has to be in the app before it creates its swap chain:
/// <list type="bullet">
/// <item>a normal app is restarted through the hook (rundll32 ...,LaunchWithGlass);</item>
/// <item>a Store (MSIX) app can't be started that way, so its package's debug settings make every launch start paused
/// and run the hook's AttachToPackageProcess first, and it is restarted through Windows.</item>
/// </list>
/// Turning this on restarts the app once, and an app started normally while it is on is restarted the same way. Areas
/// that must stay solid (pictures; LogMeIn's remote screen) are found with UI Automation and handed to the hook.
/// See docs/GLASS_WRITEUP.md, "Update 9".
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
        var rules = KeepSolidRule.For(_profile.ProcessName);
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
                        target.Dispose();
                        target = null;
                    }
                    if (target is not null && publisher?.ProcessId != target.Id)
                    {
                        publisher?.Dispose();
                        publisher = new KeepAreaPublisher(target.Id, target.MainWindowHandle, rules);
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
        if (target is null) return;
        if (Package.Of(target) is { } package) package.SetGlassAtLaunch(null);
        if (HasHook(target)) Relaunch(target, withGlass: false);
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
        var package = Package.Of(process);
        string? exe;
        try { exe = process.MainModule?.FileName; }
        catch (System.ComponentModel.Win32Exception) { exe = null; }
        if (exe is null && package is null) return;
        var dll = withGlass ? HookLibrary.Extract() : null;
        if (withGlass && dll is null) { Log("the hook DLL isn't available in this build"); return; }
        if (withGlass && package is not null && !package.SetGlassAtLaunch(dll)) return;

        // Electron and similar apps hide to the tray on close and leave helper processes; all of them go.
        var name = process.ProcessName;
        process.CloseMainWindow();
        if (!process.WaitForExit(5000))
        {
            try { process.Kill(entireProcessTree: true); process.WaitForExit(3000); }
            catch (InvalidOperationException) { }
        }
        foreach (var leftover in Process.GetProcessesByName(name))
        {
            try { leftover.Kill(); }
            catch (Exception exception) when (exception is InvalidOperationException or System.ComponentModel.Win32Exception) { }
            leftover.Dispose();
        }

        ProcessStartInfo start;
        if (package is not null)
            start = new ProcessStartInfo("explorer.exe", $"shell:AppsFolder\\{package.AppUserModelId}") { UseShellExecute = true };
        else if (withGlass)
            start = new ProcessStartInfo("rundll32.exe", $"\"{dll}\",LaunchWithGlass \"{exe}\"") { UseShellExecute = false };
        else
            start = new ProcessStartInfo(exe!) { UseShellExecute = true };
        Process.Start(start)?.Dispose();
        Log($"restarted {name} {(withGlass ? "with" : "without")} glass");
    }

    internal static void Log(string message) => Debug.WriteLine($"[dxgi glass] {message}");

    /// <summary>A Store (MSIX) app's package: how to start it, and its debug-settings switch for the hook.</summary>
    private sealed class Package
    {
        internal string FullName { get; private init; } = "";
        internal string AppUserModelId { get; private init; } = "";

        internal static Package? Of(Process process)
        {
            var handle = OpenProcess(ProcessQueryLimitedInformation, false, process.Id);
            if (handle == nint.Zero) return null;
            try
            {
                var length = 512;
                var name = new StringBuilder(length);
                if (GetPackageFullName(handle, ref length, name) != 0) return null;
                length = 512;
                var id = new StringBuilder(length);
                if (GetApplicationUserModelId(handle, ref length, id) != 0) return null;
                return new Package { FullName = name.ToString(), AppUserModelId = id.ToString() };
            }
            finally { CloseHandle(handle); }
        }

        /// <summary>With a DLL path, every launch of the package starts paused and gets the hook; null turns that off.</summary>
        internal bool SetGlassAtLaunch(string? dll)
        {
            try
            {
                var settings = (IPackageDebugSettings)new PackageDebugSettings();
                var result = dll is null
                    ? settings.DisableDebugging(FullName)
                    : settings.EnableDebugging(FullName, $"{Environment.GetFolderPath(Environment.SpecialFolder.System)}\\rundll32.exe \"{dll}\",AttachToPackageProcess", nint.Zero);
                Log($"package {FullName}: glass at launch {(dll is null ? "off" : "on")} ({result:X8})");
                return result == 0;
            }
            catch (COMException exception) { Log($"package debug settings failed: {exception.Message}"); return false; }
        }

        [ComImport, Guid("F27C3930-8029-4AD1-94E3-3DBA417810C1"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IPackageDebugSettings
        {
            [PreserveSig] int EnableDebugging([MarshalAs(UnmanagedType.LPWStr)] string packageFullName, [MarshalAs(UnmanagedType.LPWStr)] string debuggerCommandLine, nint environment);
            [PreserveSig] int DisableDebugging([MarshalAs(UnmanagedType.LPWStr)] string packageFullName);
        }

        [ComImport, Guid("B1AEC16F-2383-4852-B0E9-8F0B1DC66B4D")]
        private class PackageDebugSettings { }

        private const uint ProcessQueryLimitedInformation = 0x1000;
        [DllImport("kernel32.dll")] private static extern nint OpenProcess(uint access, bool inherit, int processId);
        [DllImport("kernel32.dll")] private static extern bool CloseHandle(nint handle);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern int GetPackageFullName(nint process, ref int length, StringBuilder name);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern int GetApplicationUserModelId(nint process, ref int length, StringBuilder id);
    }
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

/// <summary>How to find an app's must-stay-solid areas with UI Automation.</summary>
internal sealed record KeepSolidRule(string? DocumentPrefix, string? AutomationIdSuffix, bool Images)
{
    // Every app keeps its pictures solid (a photo's colors near the background would turn grey and see-through);
    // LogMeIn Resolve also keeps its remote screen, the session page's <video> element.
    internal static IReadOnlyList<KeepSolidRule> For(string processName) =>
        string.Equals(processName, "ra-technician-console", StringComparison.OrdinalIgnoreCase)
            ? [new KeepSolidRule("session", "VideoStream_Video", false), new KeepSolidRule(null, null, true)]
            : [new KeepSolidRule(null, null, true)];
}

/// <summary>
/// Publishes up to 8 keep-solid rectangles (client pixels) in Local\GlassForge.DxgiKeep.&lt;pid&gt;, where the hook reads
/// them. Found elements are reused and only their rectangles re-read; a full search (many calls into the app) runs only
/// every 3 s or after the window settles. LogMeIn's embedded Chromium stops reporting the inside of its session page
/// after full screen is toggled, so the video's insets inside the page are remembered while visible and applied later.
/// </summary>
internal sealed class KeepAreaPublisher : IDisposable
{
    private const int MaxAreas = 8;
    private const int MinPictureSize = 24;
    private readonly nint _window;
    private readonly IReadOnlyList<KeepSolidRule> _rules;
    private readonly string _processName;
    // One area list per process of the app: Chromium draws in its GPU process, not the one that owns the window.
    private readonly Dictionary<int, (MemoryMappedFile Mapping, MemoryMappedViewAccessor View)> _views = [];
    private AutomationElement? _page;
    private AutomationElement? _video;
    private List<AutomationElement> _pictures = [];
    private DateTime _searched = DateTime.MinValue;
    private double[]? _insets;
    private string _lastFrame = "";

    internal int ProcessId { get; }

    internal KeepAreaPublisher(int processId, nint window, IReadOnlyList<KeepSolidRule> rules)
    {
        ProcessId = processId;
        _window = window;
        _rules = rules;
        using var process = Process.GetProcessById(processId);
        _processName = process.ProcessName;
    }

    private void Write(int[][] areas)
    {
        foreach (var process in Process.GetProcessesByName(_processName))
        {
            using (process)
            {
                if (!_views.TryGetValue(process.Id, out var entry))
                {
                    var mapping = MemoryMappedFile.CreateOrOpen($"Local\\GlassForge.DxgiKeep.{process.Id}", 4 + MaxAreas * 16);
                    _views[process.Id] = entry = (mapping, mapping.CreateViewAccessor());
                }
                for (var i = 0; i < areas.Length; i++)
                    for (var j = 0; j < 4; j++) entry.View.Write(4 + i * 16 + j * 4, areas[i][j]);
                entry.View.Write(0, areas.Length);
            }
        }
    }

    // Chromium presents into a child window ("Intermediate D3D Window"), offset from the top-level client area; areas
    // are measured from wherever the frames are actually drawn.
    private (NativeMethods.Point Origin, NativeMethods.Rect Client) Surface()
    {
        var surface = NativeMethods.FindWindowEx(_window, nint.Zero, "Intermediate D3D Window", null);
        var target = surface != nint.Zero ? surface : _window;
        NativeMethods.GetClientRect(target, out var client);
        var origin = new NativeMethods.Point();
        NativeMethods.ClientToScreen(target, ref origin);
        return (origin, client);
    }

    internal void Update()
    {
        // While the window moves or resizes, the two coordinate readings disagree; keep what was published until it settles.
        if (!NativeMethods.GetWindowRect(_window, out var frame)) return;
        var frameText = $"{frame.Left},{frame.Top},{frame.Right},{frame.Bottom}";
        if (frameText != _lastFrame) { _lastFrame = frameText; _searched = DateTime.MinValue; return; }
        var (origin, client) = Surface();
        var search = (DateTime.Now - _searched).TotalSeconds >= 3;
        if (search) _searched = DateTime.Now;

        var areas = new List<System.Windows.Rect>();
        foreach (var rule in _rules)
        {
            try
            {
                if (rule.Images) areas.AddRange(Pictures(search));
                else if (Session(rule, search, client) is { } session) areas.Add(session);
            }
            catch (ElementNotAvailableException) { _page = null; _video = null; _pictures = []; }
            catch (COMException) { _page = null; _video = null; _pictures = []; }
        }

        // Largest first: if there are more than fit, the big ones matter.
        var published = areas
            .Select(a => new[] { (int)(a.Left - origin.X), (int)(a.Top - origin.Y), (int)(a.Right - origin.X), (int)(a.Bottom - origin.Y) })
            .Select(r => new[] { Math.Max(r[0], 0), Math.Max(r[1], 0), Math.Min(r[2], client.Right), Math.Min(r[3], client.Bottom) })
            .Where(r => r[2] - r[0] >= 1 && r[3] - r[1] >= 1)
            .OrderByDescending(r => (r[2] - r[0]) * (r[3] - r[1]))
            .Take(MaxAreas)
            .ToArray();
        Write(published);
    }

    // Pictures: every Image element the app reports (Chromium reports each <img>).
    private IEnumerable<System.Windows.Rect> Pictures(bool search)
    {
        if (search)
        {
            var images = new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Image);
            _pictures = AutomationElement.FromHandle(_window).FindAll(TreeScope.Descendants, images).Cast<AutomationElement>().ToList();
        }
        var found = new List<System.Windows.Rect>();
        foreach (var picture in _pictures)
        {
            try
            {
                var r = picture.Current.BoundingRectangle;
                if (!r.IsEmpty && r.Width >= MinPictureSize && r.Height >= MinPictureSize && !picture.Current.IsOffscreen) found.Add(r);
            }
            catch (ElementNotAvailableException) { }
        }
        return found;
    }

    // An embedded page's element (LogMeIn's remote screen), with the remembered-insets fallback.
    private System.Windows.Rect? Session(KeepSolidRule rule, bool search, NativeMethods.Rect client)
    {
        if (_page is null || search)
        {
            _page = FindPage(rule);
            _video = _page is null ? null : FindVideo(_page, rule, 0);
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
        // A real video rectangle is non-empty, inside the window and inside its page (after full screen ends, the old
        // element lingers for seconds with its full-screen size).
        if (v is { IsEmpty: false } r && r.Width >= 1 && r.Width <= client.Right + 8 && r.Height <= client.Bottom + 8
            && r.Left >= p.Left - 2 && r.Top >= p.Top - 2 && r.Right <= p.Right + 2 && r.Bottom <= p.Bottom + 2)
        {
            _insets = [r.Left - p.Left, r.Top - p.Top, p.Right - r.Right, p.Bottom - r.Bottom];
            return r;
        }
        if (_insets is { } i)
            return new System.Windows.Rect(new System.Windows.Point(p.Left + i[0], p.Top + i[1]), new System.Windows.Point(p.Right - i[2], p.Bottom - i[3]));
        return p; // nothing better known yet: the whole page stays solid
    }

    private AutomationElement? FindPage(KeepSolidRule rule)
    {
        var documents = new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Document);
        foreach (AutomationElement document in AutomationElement.FromHandle(_window).FindAll(TreeScope.Descendants, documents))
            if (document.Current.Name.StartsWith(rule.DocumentPrefix!, StringComparison.Ordinal)) return document;
        return null;
    }

    // The video element only shows up in the raw view.
    private static AutomationElement? FindVideo(AutomationElement element, KeepSolidRule rule, int depth)
    {
        if (depth > 40) return null;
        for (var child = TreeWalker.RawViewWalker.GetFirstChild(element); child is not null; child = TreeWalker.RawViewWalker.GetNextSibling(child))
        {
            if (child.Current.AutomationId.EndsWith(rule.AutomationIdSuffix!, StringComparison.Ordinal)) return child;
            if (FindVideo(child, rule, depth + 1) is { } found) return found;
        }
        return null;
    }

    public void Dispose()
    {
        foreach (var (mapping, view) in _views.Values)
        {
            view.Dispose();
            mapping.Dispose();
        }
        _views.Clear();
    }
}
