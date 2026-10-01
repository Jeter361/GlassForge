using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using GlassForge.Models;
using GlassForge.Native;

namespace GlassForge.Services;

/// <summary>
/// Full File Explorer glass (experimental). Explorer paints its own opaque backgrounds, scrollbars and text, so a
/// native backdrop alone only reaches its title bar. This loads GlassForge.Hook.dll into each File Explorer window's
/// UI thread with a thread-scoped message hook; the DLL redraws those parts for glass. On stop every window is asked
/// to restore itself before the hooks are removed.
/// </summary>
internal sealed class ExplorerHookSession : IGlassSession
{
    private Thread? _thread;
    private ExplorerHookWorker? _worker;
    private readonly ManualResetEventSlim _ready = new();

    public void Start()
    {
        _thread = new Thread(() =>
        {
            try { _worker = new ExplorerHookWorker(); }
            finally { _ready.Set(); }
            System.Windows.Forms.Application.Run(_worker.Context);
        }) { IsBackground = true, Name = "GlassForge explorer hook" };
        _thread.SetApartmentState(ApartmentState.STA);
        _thread.Start();
    }

    public void Dispose()
    {
        if (_thread is not null) _ready.Wait(2000);
        _worker?.Stop();
        _thread?.Join(3000);
        _ready.Dispose();
    }
}

internal sealed class ExplorerHookWorker
{
    private const int WhGetMessage = 3;
    private const int WmNull = 0;
    private const string ExplorerWindowClass = "CabinetWClass";
    private static readonly int AttachMessage = NativeMethods.RegisterWindowMessage("GlassForge.Hook.Attach");
    private static readonly int DetachMessage = NativeMethods.RegisterWindowMessage("GlassForge.Hook.Detach");

    private readonly nint _module;
    private readonly nint _procedure;
    // One hook per Explorer UI thread, with the window it was installed for.
    private readonly Dictionary<uint, (nint Hook, nint Window)> _threads = [];
    private readonly System.Windows.Forms.Control _marshal = new();
    private readonly System.Windows.Forms.Timer _discoveryTimer = new() { Interval = 3000 };
    private readonly NativeMethods.WinEventProc _callback;
    private nint _showHook;

    internal System.Windows.Forms.ApplicationContext Context { get; } = new();

    internal ExplorerHookWorker()
    {
        _ = _marshal.Handle; // Creates the window that Stop() marshals onto.
        _callback = OnWindowShown;
        var path = HookLibrary.Extract();
        if (path is null) return;
        // Loaded here too: SetWindowsHookEx needs the procedure's address in this process to map it into Explorer.
        _module = NativeMethods.LoadLibrary(path);
        _procedure = _module == nint.Zero ? nint.Zero : NativeMethods.GetProcAddress(_module, "GlassForgeGetMessageProc");
        if (_procedure == nint.Zero) return;
        _showHook = NativeMethods.SetWinEventHook(NativeMethods.EventObjectShow, NativeMethods.EventObjectShow,
            nint.Zero, _callback, 0, 0, NativeMethods.WineventOutOfContext);
        _discoveryTimer.Tick += (_, _) => HookExistingWindows();
        _discoveryTimer.Start();
        HookExistingWindows();
    }

    private void HookExistingWindows()
    {
        // Closed windows take their thread with them; drop those hooks so a recycled thread ID gets a fresh one.
        foreach (var (thread, entry) in _threads.Where(pair => !NativeMethods.IsWindow(pair.Value.Window)).ToArray())
        {
            NativeMethods.UnhookWindowsHookEx(entry.Hook);
            _threads.Remove(thread);
        }
        NativeMethods.EnumWindows((window, _) => { TryHook(window); return true; }, nint.Zero);
    }

    private void OnWindowShown(nint hook, uint eventType, nint window, int objectId, int childId, uint threadId, uint eventTime)
    {
        if (objectId == NativeMethods.ObjidWindow) TryHook(window);
    }

    private void TryHook(nint window)
    {
        if (!IsExplorerWindow(window)) return;
        var thread = NativeMethods.GetWindowThreadProcessId(window, out _);
        if (thread == 0 || _threads.ContainsKey(thread)) return;
        var hook = NativeMethods.SetWindowsHookEx(WhGetMessage, _procedure, _module, thread);
        if (hook == nint.Zero) return;
        _threads[thread] = (hook, window);
        // Attach clears an earlier Detach if the DLL is still loaded from a previous run; WM_NULL wakes the thread.
        NativeMethods.PostMessage(window, AttachMessage, nint.Zero, nint.Zero);
        NativeMethods.PostMessage(window, WmNull, nint.Zero, nint.Zero);
    }

    private static bool IsExplorerWindow(nint window)
    {
        if (NativeMethods.GetAncestor(window, NativeMethods.GaRoot) != window) return false;
        var name = new StringBuilder(32);
        return NativeMethods.GetClassName(window, name, name.Capacity) > 0 && name.ToString() == ExplorerWindowClass;
    }

    internal void Stop()
    {
        if (_marshal.IsDisposed) return;
        try { _marshal.Invoke(Shutdown); }
        catch (InvalidOperationException) { }
    }

    private void Shutdown()
    {
        if (_showHook != nint.Zero) NativeMethods.UnhookWinEvent(_showHook);
        _discoveryTimer.Dispose();
        // Each window restores itself on its own thread; the hooks must stay until it has, or the DLL unloads first.
        foreach (var (_, entry) in _threads) NativeMethods.PostMessage(entry.Window, DetachMessage, nint.Zero, nint.Zero);
        if (_threads.Count > 0) Thread.Sleep(800);
        foreach (var (_, entry) in _threads)
        {
            NativeMethods.UnhookWindowsHookEx(entry.Hook);
            NativeMethods.PostMessage(entry.Window, WmNull, nint.Zero, nint.Zero);
        }
        _threads.Clear();
        _marshal.Dispose();
        Context.ExitThread();
    }
}

/// <summary>The hook DLL ships inside GlassForge.exe and is written next to the profiles on first use.</summary>
internal static class HookLibrary
{
    private const string ResourceName = "GlassForge.Hook.dll";

    /// <summary>The DLL is x64 only, and debug builds made without the native project don't contain it.</summary>
    internal static bool IsAvailable =>
        Environment.Is64BitProcess && Assembly.GetExecutingAssembly().GetManifestResourceInfo(ResourceName) is not null;

    /// <summary>
    /// Writes the DLL under %LOCALAPPDATA%\GlassForge\hook named by its hash. Explorer can keep an old copy loaded
    /// (XAML diagnostics holds it until Explorer restarts), so each version gets its own file instead of overwriting.
    /// </summary>
    internal static string? Extract()
    {
        if (!IsAvailable) return null;
        using var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(ResourceName);
        if (stream is null) return null;
        using var buffer = new MemoryStream();
        stream.CopyTo(buffer);
        var bytes = buffer.ToArray();
        var hash = Convert.ToHexString(SHA256.HashData(bytes))[..12];
        var directory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "GlassForge", "hook");
        var path = Path.Combine(directory, $"GlassForge.Hook.{hash}.dll");
        try
        {
            if (!File.Exists(path))
            {
                Directory.CreateDirectory(directory);
                File.WriteAllBytes(path, bytes);
            }
            return path;
        }
        catch (IOException) { return null; }
        catch (UnauthorizedAccessException) { return null; }
    }
}
