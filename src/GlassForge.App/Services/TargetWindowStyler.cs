using GlassForge.Models;
using GlassForge.Native;

namespace GlassForge.Services;

/// <summary>
/// Applies DWM frame attributes (system backdrop, corners, title bar theme) to an app's own windows and puts
/// them back when the profile stops. These are public DWM attributes set from outside the process, so this
/// stays injection-free. Idea credit: MicaForEveryone (MIT), which styles windows the same way.
/// </summary>
internal sealed class TargetWindowStyler
{
    // DWM_SYSTEMBACKDROP_TYPE: 0 auto, 3 Acrylic. Needs Windows 11 22H2. Mica (2) and Mica Alt (4) are deliberately
    // not offered: they tint from the static wallpaper image, so they show nothing over animated wallpapers.
    private const int BackdropAuto = 0;
    private const int BackdropAcrylic = 3;
    private const int CornerDefault = 0;
    internal static bool SupportsSystemBackdrop { get; } = Environment.OSVersion.Version >= new Version(10, 0, 22621);
    internal static bool SupportsCorners { get; } = Environment.OSVersion.Version >= new Version(10, 0, 22000);

    private sealed class Original
    {
        internal int Backdrop = BackdropAuto;
        internal int Corners = CornerDefault;
        internal int? DarkMode;
        internal bool Extended;
    }

    private readonly AppProfile _profile;
    private readonly bool _applyBackdrop;
    private readonly Dictionary<nint, Original> _touched = [];

    internal TargetWindowStyler(AppProfile profile, bool applyBackdrop)
    {
        _profile = profile;
        _applyBackdrop = applyBackdrop && SupportsSystemBackdrop;
    }

    internal bool IsEmpty => !_applyBackdrop && (!SupportsCorners || _profile.Corners == CornerStyle.Default) && _profile.TitleBar == TitleBarTheme.Default;

    internal bool IsStyled(nint window) => _touched.ContainsKey(window);

    internal IEnumerable<nint> StyledWindows => _touched.Keys;

    internal bool AppliesBackdrop => _applyBackdrop;

    /// <summary>Styles a window once; DWM keeps the attributes until the window closes or they are reset.</summary>
    internal void Apply(nint window)
    {
        if (IsEmpty || _touched.ContainsKey(window)) return;
        var original = new Original();

        if (_profile.TitleBar != TitleBarTheme.Default)
        {
            if (NativeMethods.DwmGetWindowAttribute(window, NativeMethods.DwmwaUseImmersiveDarkMode, out int dark, sizeof(int)) == 0)
                original.DarkMode = dark;
            var value = _profile.TitleBar == TitleBarTheme.Dark ? 1 : 0;
            NativeMethods.DwmSetWindowAttribute(window, NativeMethods.DwmwaUseImmersiveDarkMode, ref value, sizeof(int));
        }

        if (SupportsCorners && _profile.Corners != CornerStyle.Default)
        {
            if (NativeMethods.DwmGetWindowAttribute(window, NativeMethods.DwmwaWindowCornerPreference, out int corners, sizeof(int)) == 0)
                original.Corners = corners;
            var value = CornerValue(_profile.Corners);
            NativeMethods.DwmSetWindowAttribute(window, NativeMethods.DwmwaWindowCornerPreference, ref value, sizeof(int));
        }

        if (_applyBackdrop)
        {
            if (NativeMethods.DwmGetWindowAttribute(window, NativeMethods.DwmwaSystemBackdropType, out int backdrop, sizeof(int)) == 0)
                original.Backdrop = backdrop;
            var value = BackdropAcrylic;
            NativeMethods.DwmSetWindowAttribute(window, NativeMethods.DwmwaSystemBackdropType, ref value, sizeof(int));
            // Dialogs (Save As, message boxes) paint plain GDI with no alpha; extended, their black text and
            // controls turn see-through and glow. They keep the Acrylic title bar only.
            if (_profile.ExtendIntoClient && !IsDialog(window))
            {
                // The whole window becomes "frame", so the backdrop shows anywhere the app leaves black/transparent.
                var margins = new NativeMethods.Margins { Left = -1, Right = -1, Top = -1, Bottom = -1 };
                original.Extended = NativeMethods.DwmExtendFrameIntoClientArea(window, ref margins) == 0;
            }
        }

        _touched[window] = original;
        RepaintFrame(window);
    }

    // DWM only picks up the new caption theme and backdrop on the next non-client paint; without this an
    // already-open window keeps its old title bar until it's next activated or resized. This is a one-off
    // invalidation of the frame (no style change), so it doesn't cause the WM_STYLECHANGED flicker.
    private static void RepaintFrame(nint window) =>
        NativeMethods.RedrawWindow(window, nint.Zero, nint.Zero, NativeMethods.RdwFrame | NativeMethods.RdwInvalidate);

    /// <summary>
    /// Puts the backdrop and extended frame back if the app reset them. Apps such as Notepad call
    /// DwmExtendFrameIntoClientArea themselves (on activation, resize, theme changes), which shrinks the glass back
    /// to the title bar. Margins can't be read back, so they are simply set again; the backdrop is only rewritten when
    /// it actually changed.
    /// </summary>
    internal void Reassert(nint window)
    {
        if (!ReassertBackdrop(window) || !_touched[window].Extended) return;
        var margins = new NativeMethods.Margins { Left = -1, Right = -1, Top = -1, Bottom = -1 };
        NativeMethods.DwmExtendFrameIntoClientArea(window, ref margins);
    }

    /// <summary>
    /// Puts Acrylic back if the app changed the backdrop type; a read, and a write only when needed, so it is cheap
    /// enough to run every frame. Windows Terminal, for one, re-applies its theme's backdrop on every focus change.
    /// Returns false for windows this styler doesn't own.
    /// </summary>
    internal bool ReassertBackdrop(nint window)
    {
        if (!_applyBackdrop || !_touched.ContainsKey(window)) return false;
        if (NativeMethods.DwmGetWindowAttribute(window, NativeMethods.DwmwaSystemBackdropType, out int backdrop, sizeof(int)) == 0 &&
            backdrop != BackdropAcrylic)
        {
            var value = BackdropAcrylic;
            NativeMethods.DwmSetWindowAttribute(window, NativeMethods.DwmwaSystemBackdropType, ref value, sizeof(int));
        }
        return true;
    }

    internal void Forget(nint window) => _touched.Remove(window);

    private static bool IsDialog(nint window)
    {
        var name = new System.Text.StringBuilder(16);
        return NativeMethods.GetClassName(window, name, name.Capacity) > 0 && name.ToString() == "#32770";
    }

    internal void RestoreAll()
    {
        foreach (var (window, original) in _touched)
        {
            if (!NativeMethods.IsWindow(window)) continue;
            if (original.Extended)
            {
                var margins = new NativeMethods.Margins();
                NativeMethods.DwmExtendFrameIntoClientArea(window, ref margins);
            }
            if (_applyBackdrop)
            {
                var backdrop = original.Backdrop;
                NativeMethods.DwmSetWindowAttribute(window, NativeMethods.DwmwaSystemBackdropType, ref backdrop, sizeof(int));
            }
            if (SupportsCorners && _profile.Corners != CornerStyle.Default)
            {
                var corners = original.Corners;
                NativeMethods.DwmSetWindowAttribute(window, NativeMethods.DwmwaWindowCornerPreference, ref corners, sizeof(int));
            }
            if (original.DarkMode is int dark)
                NativeMethods.DwmSetWindowAttribute(window, NativeMethods.DwmwaUseImmersiveDarkMode, ref dark, sizeof(int));
            RepaintFrame(window);
        }
        _touched.Clear();
    }

    // DWM_WINDOW_CORNER_PREFERENCE: 0 default, 1 do not round, 2 round, 3 round small.
    internal static int CornerValue(CornerStyle style) => style switch
    {
        CornerStyle.Square => 1,
        CornerStyle.Round => 2,
        CornerStyle.RoundSmall => 3,
        _ => CornerDefault
    };
}
