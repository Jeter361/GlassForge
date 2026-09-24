using System.Drawing;
using System.Globalization;
using System.Runtime.InteropServices;
using GlassForge.Models;
using GlassForge.Native;

namespace GlassForge.Services;

internal sealed class GlassHostSession : IDisposable
{
    private readonly AppProfile _profile;
    private Thread? _thread;
    private GlassHostForm? _form;

    internal GlassHostSession(AppProfile profile)
    {
        _profile = new AppProfile
        {
            Id = profile.Id,
            DisplayName = profile.DisplayName,
            ProcessName = profile.ProcessName,
            Enabled = profile.Enabled,
            WindowOpacity = profile.WindowOpacity,
            TintOpacity = profile.TintOpacity,
            TintColor = profile.TintColor
        };
    }

    internal void Start()
    {
        _thread = new Thread(() =>
        {
            NativeMethods.SetProcessDpiAwarenessContext(new nint(-4));
            System.Windows.Forms.Application.EnableVisualStyles();
            _form = new GlassHostForm(_profile);
            System.Windows.Forms.Application.Run(_form);
        }) { IsBackground = true, Name = $"GlassForge:{_profile.ProcessName}" };
        _thread.SetApartmentState(ApartmentState.STA);
        _thread.Start();
    }

    public void Dispose()
    {
        var form = _form;
        if (form is null || form.IsDisposed) return;
        try
        {
            if (form.IsHandleCreated && form.InvokeRequired)
                form.Invoke(new Action(form.Close));
            else
                form.Close();
            _thread?.Join(2000);
        }
        catch (InvalidOperationException) { }
    }
}

internal sealed class GlassHostForm : System.Windows.Forms.Form
{
    private readonly AppProfile _profile;
    private readonly System.Windows.Forms.Timer _discoveryTimer = new() { Interval = 1000 };
    private readonly System.Windows.Forms.Timer _trackingTimer = new() { Interval = 16 };
    private readonly NativeMethods.WinEventProc _locationCallback;
    private readonly NativeMethods.WinEventProc _foregroundCallback;
    private nint _locationHook;
    private nint _foregroundHook;
    private nint _target;
    private NativeMethods.Rect _lastRect;
    private bool _hasLastRect;
    private int _insetLeft, _insetTop, _insetRight, _insetBottom;
    private readonly Dictionary<nint, nint> _originalStyles = [];

    protected override System.Windows.Forms.CreateParams CreateParams
    {
        get
        {
            const int wsExToolWindow = 0x80;
            const int wsExNoActivate = 0x08000000;
            var parameters = base.CreateParams;
            parameters.ExStyle |= wsExToolWindow | wsExNoActivate;
            return parameters;
        }
    }

    protected override bool ShowWithoutActivation => true;

    internal GlassHostForm(AppProfile profile)
    {
        _profile = profile;
        _locationCallback = OnLocationChanged;
        _foregroundCallback = OnForegroundChanged;
        FormBorderStyle = System.Windows.Forms.FormBorderStyle.None;
        ShowInTaskbar = false;
        BackColor = Color.Black;
        StartPosition = System.Windows.Forms.FormStartPosition.Manual;
        Text = $"GlassForge Host · {profile.DisplayName}";
        Shown += OnShown;
    }

    protected override void OnPaintBackground(System.Windows.Forms.PaintEventArgs e) { }

    private void OnShown(object? sender, EventArgs e)
    {
        EnableAcrylic();
        _locationHook = NativeMethods.SetWinEventHook(NativeMethods.EventObjectLocationChange, NativeMethods.EventObjectLocationChange,
            nint.Zero, _locationCallback, 0, 0, NativeMethods.WineventOutOfContext);
        _foregroundHook = NativeMethods.SetWinEventHook(NativeMethods.EventSystemForeground, NativeMethods.EventSystemForeground,
            nint.Zero, _foregroundCallback, 0, 0, NativeMethods.WineventOutOfContext);
        _discoveryTimer.Tick += (_, _) => DiscoverTarget();
        _trackingTimer.Tick += (_, _) => Align(false);
        _discoveryTimer.Start();
        _trackingTimer.Start();
        DiscoverTarget();
    }

    private void EnableAcrylic()
    {
        var useHostBackdrop = 1;
        NativeMethods.DwmSetWindowAttribute(Handle, NativeMethods.DwmwaUseHostBackdropBrush, ref useHostBackdrop, sizeof(int));
        var corners = 2;
        NativeMethods.DwmSetWindowAttribute(Handle, NativeMethods.DwmwaWindowCornerPreference, ref corners, sizeof(int));
        var color = ParseAbgr(_profile.TintColor, _profile.TintOpacity);
        var accent = new NativeMethods.AccentPolicy { State = NativeMethods.AccentEnableAcrylicBlurBehind, Flags = 2, Color = color };
        var size = Marshal.SizeOf(accent);
        var pointer = Marshal.AllocHGlobal(size);
        try
        {
            Marshal.StructureToPtr(accent, pointer, false);
            var data = new NativeMethods.AttributeData { Attribute = NativeMethods.WcaAccentPolicy, Data = pointer, Size = size };
            NativeMethods.SetWindowCompositionAttribute(Handle, ref data);
        }
        finally { Marshal.FreeHGlobal(pointer); }
    }

    private static uint ParseAbgr(string rgb, byte alpha)
    {
        if (rgb.Length != 6 || !uint.TryParse(rgb, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var value)) value = 0x181818;
        var red = value >> 16 & 0xFF;
        var green = value >> 8 & 0xFF;
        var blue = value & 0xFF;
        return ((uint)alpha << 24) | (blue << 16) | (green << 8) | red;
    }

    private void DiscoverTarget()
    {
        _target = WindowDiscoveryService.FindLargestWindow(_profile.ProcessName);
        if (_target == nint.Zero) { Hide(); return; }
        if (NativeMethods.GetWindowRect(_target, out var windowRect)
            && NativeMethods.DwmGetWindowAttribute(_target, NativeMethods.DwmwaExtendedFrameBounds, out var frameRect, Marshal.SizeOf<NativeMethods.Rect>()) == 0)
        {
            _insetLeft = frameRect.Left - windowRect.Left;
            _insetTop = frameRect.Top - windowRect.Top;
            _insetRight = windowRect.Right - frameRect.Right;
            _insetBottom = windowRect.Bottom - frameRect.Bottom;
        }
        Align(true);
    }

    private void OnLocationChanged(nint hook, uint eventType, nint window, int objectId, int childId, uint threadId, uint eventTime)
    {
        if (window == _target && objectId == NativeMethods.ObjidWindow) Align(false);
    }

    private void OnForegroundChanged(nint hook, uint eventType, nint window, int objectId, int childId, uint threadId, uint eventTime)
    {
        if (window == _target) Align(true);
    }

    private void Align(bool force)
    {
        if (_target == nint.Zero || !NativeMethods.IsWindow(_target) || !NativeMethods.GetWindowRect(_target, out var rect)) return;
        rect.Left += _insetLeft;
        rect.Top += _insetTop;
        rect.Right -= _insetRight;
        rect.Bottom -= _insetBottom;
        if (!force && _hasLastRect && Equal(rect, _lastRect)) return;
        _lastRect = rect;
        _hasLastRect = true;
        var width = rect.Right - rect.Left;
        var height = rect.Bottom - rect.Top;
        if (width <= 0 || height <= 0) { Hide(); return; }

        var currentStyle = NativeMethods.GetWindowLongPtr(_target, NativeMethods.GwlExStyle);
        _originalStyles.TryAdd(_target, currentStyle);
        var style = currentStyle.ToInt64();
        NativeMethods.SetWindowLongPtr(_target, NativeMethods.GwlExStyle, new nint(style | NativeMethods.WsExLayered));
        NativeMethods.SetLayeredWindowAttributes(_target, 0, _profile.WindowOpacity, NativeMethods.LwaAlpha);
        if (!Visible) Show();
        NativeMethods.SetWindowPos(Handle, _target, rect.Left, rect.Top, width, height, NativeMethods.SwpNoActivate | NativeMethods.SwpShowWindow);
    }

    private static bool Equal(NativeMethods.Rect left, NativeMethods.Rect right) =>
        left.Left == right.Left && left.Top == right.Top && left.Right == right.Right && left.Bottom == right.Bottom;

    protected override void Dispose(bool disposing)
    {
        if (_locationHook != nint.Zero) NativeMethods.UnhookWinEvent(_locationHook);
        if (_foregroundHook != nint.Zero) NativeMethods.UnhookWinEvent(_foregroundHook);
        if (disposing)
        {
            _discoveryTimer.Dispose();
            _trackingTimer.Dispose();
            foreach (var (window, originalStyle) in _originalStyles)
            {
                if (!NativeMethods.IsWindow(window)) continue;
                NativeMethods.SetLayeredWindowAttributes(window, 0, 255, NativeMethods.LwaAlpha);
                NativeMethods.SetWindowLongPtr(window, NativeMethods.GwlExStyle, originalStyle);
            }
            _originalStyles.Clear();
        }
        base.Dispose(disposing);
    }
}
