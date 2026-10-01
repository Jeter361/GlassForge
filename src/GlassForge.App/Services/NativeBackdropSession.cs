using GlassForge.Models;
using GlassForge.Native;

namespace GlassForge.Services;

/// <summary>
/// Native mode: asks DWM to draw the backdrop on every top-level window of the app. No host window, no
/// tracking timer and no layered alpha, so there is nothing to flicker; the trade-off is that the glass only
/// shows where the app doesn't paint its own background (title bars, and apps built for Mica).
/// </summary>
internal sealed class NativeBackdropSession : IGlassSession
{
    private readonly AppProfile _profile;
    private Thread? _thread;
    private NativeBackdropWorker? _worker;
    private readonly ManualResetEventSlim _ready = new();

    internal NativeBackdropSession(AppProfile profile) => _profile = profile.Clone();

    public void Start()
    {
        _thread = new Thread(() =>
        {
            NativeMethods.SetThreadDpiAwarenessContext(NativeMethods.DpiAwarenessContextPerMonitorAwareV2);
            try { _worker = new NativeBackdropWorker(_profile); }
            finally { _ready.Set(); }
            System.Windows.Forms.Application.Run(_worker.Context);
        }) { IsBackground = true, Name = $"GlassForge native:{_profile.ProcessName}" };
        _thread.SetApartmentState(ApartmentState.STA);
        _thread.Start();
    }

    public void Dispose()
    {
        // Stop can race a session that is still starting; wait for its worker so the windows get restored.
        if (_thread is not null) _ready.Wait(2000);
        _worker?.Stop();
        _thread?.Join(2000);
        _ready.Dispose();
    }
}

internal sealed class NativeBackdropWorker
{
    private readonly AppProfile _profile;
    private readonly TargetWindowStyler _styler;
    private readonly System.Windows.Forms.Control _marshal = new();
    private readonly System.Windows.Forms.Timer _discoveryTimer = new() { Interval = 3000 };
    private readonly NativeMethods.WinEventProc _callback;
    private readonly List<nint> _hooks = [];
    private readonly Dictionary<uint, bool> _processMatches = [];
    private HashSet<uint> _processIds = [];

    internal System.Windows.Forms.ApplicationContext Context { get; } = new();

    internal NativeBackdropWorker(AppProfile profile)
    {
        _profile = profile;
        _styler = new TargetWindowStyler(profile, applyBackdrop: true);
        _callback = OnWindowEvent;
        _ = _marshal.Handle; // Creates the window that Stop() marshals onto.
        // Show catches new windows the moment they appear (the same hook MicaForEveryone uses); destroy lets the
        // styler drop handles that Windows may recycle. The timer is only a safety net.
        _hooks.Add(NativeMethods.SetWinEventHook(NativeMethods.EventObjectShow, NativeMethods.EventObjectShow,
            nint.Zero, _callback, 0, 0, NativeMethods.WineventOutOfContext));
        _hooks.Add(NativeMethods.SetWinEventHook(NativeMethods.EventObjectDestroy, NativeMethods.EventObjectDestroy,
            nint.Zero, _callback, 0, 0, NativeMethods.WineventOutOfContext));
        _discoveryTimer.Tick += (_, _) => StyleExistingWindows();
        _discoveryTimer.Start();
        StyleExistingWindows();
    }

    private void StyleExistingWindows()
    {
        _processIds = WindowDiscoveryService.GetProcessIds(_profile.ProcessName);
        _processMatches.Clear();
        var ids = _processIds;
        NativeMethods.EnumWindows((window, _) =>
        {
            if (NativeMethods.GetWindowThreadProcessId(window, out var processId) != 0 && ids.Contains(processId)) TryStyle(window);
            return true;
        }, nint.Zero);
    }

    private void OnWindowEvent(nint hook, uint eventType, nint window, int objectId, int childId, uint threadId, uint eventTime)
    {
        if (objectId != NativeMethods.ObjidWindow) return;
        if (eventType == NativeMethods.EventObjectDestroy) { _styler.Forget(window); return; }
        if (_styler.IsStyled(window) || NativeMethods.GetAncestor(window, NativeMethods.GaRoot) != window) return;
        if (NativeMethods.GetWindowThreadProcessId(window, out var processId) == 0) return;
        if (!_processIds.Contains(processId))
        {
            if (!_processMatches.TryGetValue(processId, out var matches))
                _processMatches[processId] = matches = WindowDiscoveryService.IsProcessNamed(processId, _profile.ProcessName);
            if (!matches) return;
            _processIds.Add(processId);
        }
        TryStyle(window);
    }

    // Only framed app windows: tooltips, menus and overlays would get a stray backdrop.
    private void TryStyle(nint window)
    {
        if (WindowDiscoveryService.IsFramedAppWindow(window)) _styler.Apply(window);
    }

    internal void Stop()
    {
        if (_marshal.IsDisposed) return;
        try { _marshal.Invoke(Shutdown); }
        catch (InvalidOperationException) { }
    }

    private void Shutdown()
    {
        foreach (var hook in _hooks) if (hook != nint.Zero) NativeMethods.UnhookWinEvent(hook);
        _hooks.Clear();
        _discoveryTimer.Dispose();
        _styler.RestoreAll();
        _marshal.Dispose();
        Context.ExitThread();
    }
}
