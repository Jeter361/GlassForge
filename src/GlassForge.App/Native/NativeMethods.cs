using System.Runtime.InteropServices;
using System.Text;

namespace GlassForge.Native;

internal static class NativeMethods
{
    internal delegate bool EnumWindowsProc(nint window, nint parameter);
    internal delegate void WinEventProc(nint hook, uint eventType, nint window, int objectId, int childId, uint threadId, uint eventTime);

    [StructLayout(LayoutKind.Sequential)]
    internal struct Rect
    {
        internal int Left;
        internal int Top;
        internal int Right;
        internal int Bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct AccentPolicy
    {
        internal int State;
        internal int Flags;
        internal uint Color;
        internal int AnimationId;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct MonitorInfo
    {
        internal int Size;
        internal Rect Monitor;
        internal Rect Work;
        internal uint Flags;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct AttributeData
    {
        internal int Attribute;
        internal nint Data;
        internal int Size;
    }

    internal const int GwlExStyle = -20;
    internal const long WsExLayered = 0x00080000;
    internal const uint LwaAlpha = 2;
    internal const uint GaRoot = 2;
    internal const int DwmwaExtendedFrameBounds = 9;
    internal const int DwmwaCloaked = 14;
    internal const long WsExToolWindow = 0x00000080;
    internal const int WmDpiChanged = 0x02E0;
    internal static readonly nint DpiAwarenessContextPerMonitorAwareV2 = new(-4);
    internal const int DwmwaUseHostBackdropBrush = 17;
    internal const int DwmwaWindowCornerPreference = 33;
    internal const int AccentEnableAcrylicBlurBehind = 4;
    internal const int WcaAccentPolicy = 19;
    internal const uint EventSystemForeground = 0x0003;
    internal const uint EventObjectLocationChange = 0x800B;
    internal const uint EventSystemMinimizeStart = 0x0016;
    internal const uint EventSystemMinimizeEnd = 0x0017;
    internal const uint EventObjectDestroy = 0x8001;
    internal const uint EventObjectHide = 0x8003;
    internal const uint EventObjectCloaked = 0x8017;
    internal const uint EventObjectUncloaked = 0x8018;
    internal const uint WineventOutOfContext = 0;
    internal const int ObjidWindow = 0;
    internal const uint SwpNoActivate = 0x0010;
    internal const uint SwpShowWindow = 0x0040;

    [DllImport("user32.dll")] internal static extern bool EnumWindows(EnumWindowsProc callback, nint parameter);
    [DllImport("user32.dll")] internal static extern bool IsWindowVisible(nint window);
    [DllImport("user32.dll")] internal static extern bool IsWindow(nint window);
    [DllImport("user32.dll")] internal static extern bool IsIconic(nint window);
    [DllImport("user32.dll")] internal static extern bool GetWindowRect(nint window, out Rect rect);
    [DllImport("user32.dll")] internal static extern nint MonitorFromWindow(nint window, uint flags);
    [DllImport("user32.dll")] internal static extern bool GetMonitorInfo(nint monitor, ref MonitorInfo info);
    [DllImport("user32.dll")] internal static extern uint GetWindowThreadProcessId(nint window, out uint processId);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] internal static extern int GetWindowText(nint window, StringBuilder text, int count);
    [DllImport("user32.dll")] internal static extern bool SetWindowPos(nint window, nint insertAfter, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")] internal static extern bool SetLayeredWindowAttributes(nint window, uint key, byte alpha, uint flags);
    [DllImport("user32.dll")] internal static extern bool GetLayeredWindowAttributes(nint window, out uint key, out byte alpha, out uint flags);
    [DllImport("user32.dll")] internal static extern nint GetAncestor(nint window, uint flags);
    [DllImport("user32.dll")] internal static extern int SetWindowCompositionAttribute(nint window, ref AttributeData data);
    [DllImport("user32.dll")] internal static extern nint SetWinEventHook(uint eventMin, uint eventMax, nint module, WinEventProc callback, uint processId, uint threadId, uint flags);
    [DllImport("user32.dll")] internal static extern bool UnhookWinEvent(nint hook);
    [DllImport("user32.dll")] internal static extern bool SetProcessDpiAwarenessContext(nint value);
    [DllImport("user32.dll")] internal static extern nint SetThreadDpiAwarenessContext(nint value);
    [DllImport("dwmapi.dll")] internal static extern int DwmGetWindowAttribute(nint window, int attribute, out int value, int size);
    [DllImport("dwmapi.dll")] internal static extern int DwmGetWindowAttribute(nint window, int attribute, out Rect value, int size);
    [DllImport("dwmapi.dll")] internal static extern int DwmSetWindowAttribute(nint window, int attribute, ref int value, int size);

    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")]
    private static extern nint GetWindowLongPtr64(nint window, int index);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongW")]
    private static extern nint GetWindowLongPtr32(nint window, int index);
    [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW")]
    private static extern nint SetWindowLongPtr64(nint window, int index, nint value);
    [DllImport("user32.dll", EntryPoint = "SetWindowLongW")]
    private static extern nint SetWindowLongPtr32(nint window, int index, nint value);

    internal static nint GetWindowLongPtr(nint window, int index) =>
        nint.Size == 8 ? GetWindowLongPtr64(window, index) : GetWindowLongPtr32(window, index);

    internal static nint SetWindowLongPtr(nint window, int index, nint value) =>
        nint.Size == 8 ? SetWindowLongPtr64(window, index, value) : SetWindowLongPtr32(window, index, value);
}

