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
    }
}
