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
    // Notepad's text area (a RichEditD2DPT control per tab) and the background each one had before.
    private readonly Dictionary<nint, nint> _editors = [];
    private const string NotepadEditorClass = "RichEditD2DPT";
    private const int NotepadEditorBackground = 0x272727; // dark theme default, restored if the original was lost
    private readonly System.Windows.Forms.Timer _settleTimer = new() { Interval = 500 };
    // Apps that re-apply their own backdrop on focus changes (Terminal) do it a few milliseconds after the foreground
    // event; checking every frame for a moment afterwards puts Acrylic back before DWM visibly fades to theirs.
    private readonly System.Windows.Forms.Timer _guardTimer = new() { Interval = 15 };
    private long _guardUntil;
    private int _settleTicks;

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
        if (_styler.AppliesBackdrop || KeepsGlass)
            _hooks.Add(NativeMethods.SetWinEventHook(NativeMethods.EventSystemForeground, NativeMethods.EventSystemForeground,
                nint.Zero, _callback, 0, 0, NativeMethods.WineventOutOfContext));
        // Apps reset an extended frame on activation and resize; location changes catch the resize.
        if (_profile.ExtendIntoClient)
            _hooks.Add(NativeMethods.SetWinEventHook(NativeMethods.EventObjectLocationChange, NativeMethods.EventObjectLocationChange,
                nint.Zero, _callback, 0, 0, NativeMethods.WineventOutOfContext));
        _discoveryTimer.Tick += (_, _) => StyleExistingWindows();
        _guardTimer.Tick += (_, _) =>
        {
            foreach (var window in _styler.StyledWindows) _styler.ReassertBackdrop(window);
            if (Environment.TickCount64 > _guardUntil) _guardTimer.Stop();
        };
        _settleTimer.Tick += (_, _) =>
        {
            foreach (var window in _styler.StyledWindows)
            {
                _styler.Reassert(window);
                ClearEditorBackgrounds(window);
            }
            if (--_settleTicks <= 0) _settleTimer.Stop();
        };
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
        // New tabs bring new text controls; closed tabs leave handles behind.
        foreach (var editor in _editors.Keys.Where(editor => !NativeMethods.IsWindow(editor)).ToArray()) _editors.Remove(editor);
        foreach (var window in _styler.StyledWindows)
        {
            _styler.Reassert(window);
            ClearEditorBackgrounds(window);
        }
    }

    private void OnWindowEvent(nint hook, uint eventType, nint window, int objectId, int childId, uint threadId, uint eventTime)
    {
        if (eventType == NativeMethods.EventSystemForeground)
        {
            if (KeepsGlass) KeepFramesActive(window);
            // Every focus change, not just ours: an app resets its backdrop on losing focus as well as gaining it.
            Guard();
            foreach (var styled in _styler.StyledWindows) _styler.Reassert(styled);
            if (_styler.IsStyled(window)) Settle();
            return;
        }
        if (objectId != NativeMethods.ObjidWindow) return;
        if (eventType == NativeMethods.EventObjectLocationChange)
        {
            if (_styler.IsStyled(window)) _styler.Reassert(window);
            return;
        }
        if (eventType == NativeMethods.EventObjectDestroy) { _styler.Forget(window); return; }
        var root = NativeMethods.GetAncestor(window, NativeMethods.GaRoot);
        if (root != window)
        {
            if (_styler.IsStyled(root) && IsClass(window, NotepadEditorClass)) Settle();
            return;
        }
        if (_styler.IsStyled(window)) return;
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
        if (!WindowDiscoveryService.IsFramedAppWindow(window) || _styler.IsStyled(window)) return;
        _styler.Apply(window);
        ClearEditorBackgrounds(window);
        Settle();
        if (KeepsGlass && window != NativeMethods.GetForegroundWindow()) SetFrameActive(window, true);
    }

    private void Guard()
    {
        if (!_styler.AppliesBackdrop || !_styler.StyledWindows.Any()) return;
        _guardUntil = Environment.TickCount64 + 1500;
        _guardTimer.Start();
    }

    private bool ClearsEditors => _profile.IsNotepad && _profile.ExtendIntoClient && TargetWindowStyler.SupportsSystemBackdrop;

    // With HDR on, DWM composes the cleared text area straight rather than additively, so Notepad's text (drawn with
    // alpha 0) turns as see-through as the background. The opt-in fix leaves the text area alone and glass around it.
    private bool KeepsEditorsSolid => ClearsEditors && _profile.SolidTextArea;

    // A burst of re-applies over the next 5 s: apps reset their frame (and Notepad its text area) a moment after
    // appearing or activating, often after the event that told us about it.
    private void Settle()
    {
        if (!_profile.ExtendIntoClient || !_styler.AppliesBackdrop) return;
        _settleTicks = 10;
        _settleTimer.Start();
    }

    // Notepad paints its text area #272727 with no alpha, which DWM adds onto the glass as a milky haze. Black is the
    // see-through color inside an extended frame, so the text area then matches the glass around it. Only with the
    // frame extended: anywhere else black would just be black. Re-applied on every pass, because Notepad sets its
    // theme color again after the control appears (and on theme changes); EM_SETBKGNDCOLOR can't be read without
    // writing, so whatever non-black color it hands back is the one to restore.
    private void ClearEditorBackgrounds(nint window)
    {
        if (!ClearsEditors) return;
        if (KeepsEditorsSolid)
        {
            _styler.SetInset(window, VisibleEditorInset(window));
            return;
        }
        NativeMethods.EnumChildWindows(window, (child, _) =>
        {
            if (!IsClass(child, NotepadEditorClass) || !SetEditorBackground(child, 0, out var previous)) return true;
            // A brand-new control still has the system window color (white) until Notepad themes it: in dark mode that
            // isn't Notepad's color, and restoring it would leave a glowing white text area.
            var untouched = previous == (nint)NativeMethods.GetSysColor(NativeMethods.ColorWindow) && AppsUseDarkTheme();
            if (previous != 0 && !untouched) _editors[child] = previous;
            else _editors.TryAdd(child, NotepadEditorBackground);
            return true;
        }, nint.Zero);
    }

    // The current tab's text area as distances from each client edge; the inactive tabs' controls are hidden. Null when
    // no text area shows (Notepad's settings page), which keeps the whole window glass.
    private static NativeMethods.Margins? VisibleEditorInset(nint window)
    {
        var editor = nint.Zero;
        NativeMethods.EnumChildWindows(window, (child, _) =>
        {
            if (!IsClass(child, NotepadEditorClass) || !NativeMethods.IsWindowVisible(child)) return true;
            editor = child;
            return false;
        }, nint.Zero);
        if (editor == nint.Zero || !NativeMethods.GetWindowRect(editor, out var text) || !NativeMethods.GetClientRect(window, out var client))
            return null;
        var origin = new NativeMethods.Point();
        NativeMethods.ClientToScreen(window, ref origin);
        return new NativeMethods.Margins
        {
            Left = Math.Max(0, text.Left - origin.X),
            Top = Math.Max(0, text.Top - origin.Y),
            Right = Math.Max(0, client.Right - (text.Right - origin.X)),
            Bottom = Math.Max(0, client.Bottom - (text.Bottom - origin.Y)),
        };
    }

    // EM_SETBKGNDCOLOR returns the color it replaces; a hung Notepad is skipped rather than freezing this thread.
    private static bool SetEditorBackground(nint editor, nint color, out nint previous) =>
        NativeMethods.SendMessageTimeout(editor, NativeMethods.EmSetBkgndColor, 0, color, NativeMethods.SmtoAbortIfHung, 1000, out previous) != 0;

    private static bool AppsUseDarkTheme()
    {
        using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize");
        return key?.GetValue("AppsUseLightTheme") is int light && light == 0;
    }

    private static bool IsClass(nint window, string name)
    {
        var text = new System.Text.StringBuilder(32);
        return NativeMethods.GetClassName(window, text, text.Capacity) > 0 && text.ToString() == name;
    }

    private bool KeepsGlass => _profile.KeepGlassWhenUnfocused && _styler.AppliesBackdrop;

    // Windows swaps Acrylic for a flat fill once a window is deactivated. Deactivation arrives as WM_NCACTIVATE(FALSE);
    // answering it with WM_NCACTIVATE(TRUE) makes DWM draw the frame, and so the backdrop, as active again. This only
    // changes how the frame is drawn: keyboard focus and the real foreground window are untouched.
    private void KeepFramesActive(nint foreground)
    {
        foreach (var window in _styler.StyledWindows)
            if (window != foreground) SetFrameActive(window, true);
    }

    private static void SetFrameActive(nint window, bool active) =>
        NativeMethods.PostMessage(window, NativeMethods.WmNcActivate, active ? 1 : 0, nint.Zero);

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
        _settleTimer.Dispose();
        _guardTimer.Dispose();
        if (KeepsGlass)
        {
            var foreground = NativeMethods.GetForegroundWindow();
            foreach (var window in _styler.StyledWindows)
                if (window != foreground && NativeMethods.IsWindow(window)) SetFrameActive(window, false);
        }
        foreach (var (editor, previous) in _editors)
            if (NativeMethods.IsWindow(editor)) SetEditorBackground(editor, previous, out _);
        _editors.Clear();
        _styler.RestoreAll();
        _marshal.Dispose();
        Context.ExitThread();
    }
}
