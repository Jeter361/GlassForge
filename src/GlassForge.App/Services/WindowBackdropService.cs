using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;

namespace GlassForge.Services;

internal static class WindowBackdropService
{
    [StructLayout(LayoutKind.Sequential)]
    private struct Margins { internal int Left, Right, Top, Bottom; }

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(nint window, int attribute, ref int value, int size);

    [DllImport("dwmapi.dll")]
    private static extern int DwmExtendFrameIntoClientArea(nint window, ref Margins margins);

    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")] private static extern nint GetWindowLongPtr(nint window, int index);
    [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW")] private static extern nint SetWindowLongPtr(nint window, int index, nint value);
    [DllImport("user32.dll")] private static extern bool SetWindowPos(nint window, nint after, int x, int y, int cx, int cy, uint flags);

    internal static void Apply(Window window)
    {
        var handle = new WindowInteropHelper(window).Handle;
        if (handle == nint.Zero) return;

        const int dwmwaUseImmersiveDarkMode = 20;
        const int dwmwaSystemBackdropType = 38;
        const int dwmSystemBackdropTransientWindow = 3;
        var enabled = 1;
        var backdrop = dwmSystemBackdropTransientWindow;
        DwmSetWindowAttribute(handle, dwmwaUseImmersiveDarkMode, ref enabled, sizeof(int));
        DwmSetWindowAttribute(handle, dwmwaSystemBackdropType, ref backdrop, sizeof(int));
        var margins = new Margins { Left = -1, Right = -1, Top = -1, Bottom = -1 };
        DwmExtendFrameIntoClientArea(handle, ref margins);
        HideNativeCaptionButtons(handle);
    }

    // With glass extended over the title bar, DWM draws its own minimize/maximize/close underneath ours, slightly
    // offset. It only does that for windows with a system menu, so the style goes (Alt+Space loses the menu; our
    // buttons, snapping and the taskbar keep working).
    private static void HideNativeCaptionButtons(nint handle)
    {
        const int gwlStyle = -16;
        const long wsSysMenu = 0x00080000;
        var style = GetWindowLongPtr(handle, gwlStyle).ToInt64();
        if ((style & wsSysMenu) == 0) return;
        SetWindowLongPtr(handle, gwlStyle, new nint(style & ~wsSysMenu));
        const uint noMoveSizeZorderActivate = 0x0002 | 0x0001 | 0x0004 | 0x0010, frameChanged = 0x0020;
        SetWindowPos(handle, nint.Zero, 0, 0, 0, 0, noMoveSizeZorderActivate | frameChanged);
    }
}
