using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace GlassForge.Services;

/// <summary>
/// Real app icons for the profile and running-app lists: the exe's own icon at 48 px, or the window's icon when the
/// exe has none (some packaged apps). Cached per path; null means "no icon, show the letter tile".
/// </summary>
internal static class AppIconService
{
    private const int IconSize = 48;
    private static readonly Dictionary<string, ImageSource?> Cache = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>Full path of a running process's exe, or null (exited, or access denied).</summary>
    internal static string? ExecutablePath(int processId)
    {
        var process = OpenProcess(ProcessQueryLimitedInformation, false, processId);
        if (process == nint.Zero) return null;
        try
        {
            var path = new StringBuilder(1024);
            var size = path.Capacity;
            return QueryFullProcessImageName(process, 0, path, ref size) ? path.ToString() : null;
        }
        finally { CloseHandle(process); }
    }

    /// <summary>Path of the first running process with this name, for profiles whose app is open.</summary>
    internal static string? FindExecutable(string processName)
    {
        foreach (var process in Process.GetProcessesByName(processName))
            using (process)
                if (ExecutablePath(process.Id) is { } path) return path;
        return null;
    }

    /// <summary>
    /// UWP apps (Settings, Calculator…) run inside ApplicationFrameHost.exe, whose own icon is a generic frame. Their
    /// window carries the real app's identity (AppUserModelID); the shell's Apps folder has its proper icon.
    /// </summary>
    internal static ImageSource? ForHostedApp(int processId, string windowTitle)
    {
        var key = $"aumid:{processId}:{windowTitle}";
        if (Cache.TryGetValue(key, out var cached)) return cached;
        ImageSource? icon = null;
        EnumWindows((window, _) =>
        {
            GetWindowThreadProcessId(window, out var owner);
            if (owner != processId || !IsWindowVisible(window) || WindowText(window) != windowTitle) return true;
            if ((HostedProcessAppId(window) ?? AppUserModelId(window)) is { } id) icon = FromShellItem(@"shell:AppsFolder\" + id);
            return false;
        }, nint.Zero);
        icon ??= FromWindow(processId);
        Cache[key] = icon;
        return icon;
    }

    private static readonly Dictionary<string, string?> Names = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>
    /// The app's own name ("Notepad", "Microsoft Teams") from its exe's version info, or null when it has none.
    /// Window titles change with the open document, so they make poor names for a profile.
    /// </summary>
    internal static string? FriendlyName(string? executablePath)
    {
        if (string.IsNullOrEmpty(executablePath)) return null;
        if (Names.TryGetValue(executablePath, out var cached)) return cached;
        string? name = Path.GetFileNameWithoutExtension(executablePath).ToLowerInvariant() switch
        {
            "explorer" => "File Explorer",          // "Windows Explorer"
            "windowsterminal" => "Windows Terminal", // "Windows Terminal Host"
            _ => null
        };
        if (name is not null) return Names[executablePath] = name;
        try
        {
            var description = FileVersionInfo.GetVersionInfo(executablePath).FileDescription?.Trim();
            if (!string.IsNullOrEmpty(description))
                name = description.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ? description[..^4] : description;
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or ArgumentException) { }
        return Names[executablePath] = name;
    }

    internal static ImageSource? For(string? executablePath, int processId = 0)
    {
        if (string.IsNullOrEmpty(executablePath)) return processId != 0 ? FromWindow(processId) : null;
        if (Cache.TryGetValue(executablePath, out var cached)) return cached;
        // Packaged apps (Settings, Terminal, Notepad) have a Start menu icon; their exe's own may be generic or absent.
        var packaged = processId != 0 ? ProcessAppId(processId) : null;
        var icon = (packaged is null ? null : FromShellItem(@"shell:AppsFolder\" + packaged))
            ?? FromFile(executablePath) ?? (processId != 0 ? FromWindow(processId) : null);
        Cache[executablePath] = icon;
        return icon;
    }

    private static ImageSource? FromFile(string path)
    {
        if (!File.Exists(path)) return null;
        // nIconSize: large size in the low word, small in the high word; only the large one is used.
        if (SHDefExtractIcon(path, 0, 0, out var large, out var small, IconSize | (16 << 16)) != 0) return null;
        if (small != nint.Zero) DestroyIcon(small);
        return ToImage(large, destroy: true);
    }

    // Packaged apps whose exe carries no icon still give their windows one.
    private static ImageSource? FromWindow(int processId)
    {
        nint found = nint.Zero;
        EnumWindows((window, _) =>
        {
            GetWindowThreadProcessId(window, out var owner);
            if (owner != processId || !IsWindowVisible(window)) return true;
            // A hung app is skipped rather than freezing the UI thread.
            SendMessageTimeout(window, WmGetIcon, IconBig, 0, SmtoAbortIfHung, 200, out var icon);
            if (icon == nint.Zero) icon = GetClassLongPtr(window, GclpHIcon);
            if (icon == nint.Zero) return true;
            found = icon;
            return false;
        }, nint.Zero);
        // Window icons belong to their window: copy, never destroy.
        return found == nint.Zero ? null : ToImage(found, destroy: false);
    }

    private static string WindowText(nint window)
    {
        var text = new StringBuilder(512);
        GetWindowText(window, text, text.Capacity);
        return text.ToString();
    }

    // The frame window itself carries no app identity; the hosted app's CoreWindow child belongs to the real app
    // process (SystemSettings.exe for Settings), whose package identity is the AppUserModelID.
    private static string? HostedProcessAppId(nint frame)
    {
        var core = FindWindowEx(frame, nint.Zero, "Windows.UI.Core.CoreWindow", null);
        if (core == nint.Zero) return null;
        GetWindowThreadProcessId(core, out var processId);
        return ProcessAppId(processId);
    }

    // The package identity (AppUserModelID) of a packaged app's process; null for classic desktop apps.
    private static string? ProcessAppId(int processId)
    {
        var process = OpenProcess(ProcessQueryLimitedInformation, false, processId);
        if (process == nint.Zero) return null;
        try
        {
            var id = new StringBuilder(512);
            var length = id.Capacity;
            return GetApplicationUserModelId(process, ref length, id) == 0 ? id.ToString() : null;
        }
        finally { CloseHandle(process); }
    }

    private static string? AppUserModelId(nint window)
    {
        if (SHGetPropertyStoreForWindow(window, typeof(IPropertyStore).GUID, out var store) != 0) return null;
        try
        {
            var key = new PropertyKey { FormatId = new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3"), PropertyId = 5 };
            if (store.GetValue(ref key, out var value) != 0) return null;
            try { return value.Type == 31 /* VT_LPWSTR */ ? Marshal.PtrToStringUni(value.Pointer) : null; }
            finally { PropVariantClear(ref value); }
        }
        finally { Marshal.ReleaseComObject(store); }
    }

    // The shell renders the icon at the size asked for, with alpha, as a 32-bit DIB.
    private static ImageSource? FromShellItem(string parsingName)
    {
        if (SHCreateItemFromParsingName(parsingName, nint.Zero, typeof(IShellItemImageFactory).GUID, out var factory) != 0) return null;
        try
        {
            const int iconOnly = 0x4;
            if (factory.GetImage(new NativeSize { Width = IconSize, Height = IconSize }, iconOnly, out var bitmap) != 0) return null;
            try { return FromDib(bitmap); }
            finally { DeleteObject(bitmap); }
        }
        finally { Marshal.ReleaseComObject(factory); }
    }

    private static ImageSource? FromDib(nint bitmap)
    {
        var info = new BitmapInfoHeader { Size = Marshal.SizeOf<BitmapInfoHeader>(), Width = IconSize, Height = -IconSize, Planes = 1, BitCount = 32 };
        var pixels = new byte[IconSize * IconSize * 4];
        var screen = GetDC(nint.Zero);
        try { if (GetDIBits(screen, bitmap, 0, IconSize, pixels, ref info, 0) == 0) return null; }
        finally { ReleaseDC(nint.Zero, screen); }
        // Shell images are premultiplied BGRA, top-down once Height is negative.
        var image = BitmapSource.Create(IconSize, IconSize, 96, 96, PixelFormats.Pbgra32, null, pixels, IconSize * 4);
        image.Freeze();
        return image;
    }

    private static ImageSource? ToImage(nint icon, bool destroy)
    {
        if (icon == nint.Zero) return null;
        try
        {
            var image = Imaging.CreateBitmapSourceFromHIcon(icon, Int32Rect.Empty, BitmapSizeOptions.FromEmptyOptions());
            image.Freeze();
            return image;
        }
        catch (COMException) { return null; }
        finally { if (destroy) DestroyIcon(icon); }
    }

    [StructLayout(LayoutKind.Sequential)] private struct NativeSize { public int Width, Height; }
    [StructLayout(LayoutKind.Sequential)] private struct PropertyKey { public Guid FormatId; public int PropertyId; }
    [StructLayout(LayoutKind.Explicit)] private struct PropVariant { [FieldOffset(0)] public ushort Type; [FieldOffset(8)] public nint Pointer; }
    [StructLayout(LayoutKind.Sequential)]
    private struct BitmapInfoHeader
    {
        public int Size, Width, Height; public short Planes, BitCount; public int Compression, SizeImage, XPelsPerMeter, YPelsPerMeter, ClrUsed, ClrImportant;
    }

    [ComImport, Guid("bcc18b79-ba16-442f-80c4-8a59c30c463b"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IShellItemImageFactory { [PreserveSig] int GetImage(NativeSize size, int flags, out nint bitmap); }

    [ComImport, Guid("886d8eeb-8cf2-4446-8d02-cdba1dbdcf99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IPropertyStore
    {
        [PreserveSig] int GetCount(out uint count);
        [PreserveSig] int GetAt(uint index, out PropertyKey key);
        [PreserveSig] int GetValue(ref PropertyKey key, out PropVariant value);
        [PreserveSig] int SetValue(ref PropertyKey key, ref PropVariant value);
        [PreserveSig] int Commit();
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern int SHCreateItemFromParsingName(string path, nint bindContext, [MarshalAs(UnmanagedType.LPStruct)] Guid iid, out IShellItemImageFactory factory);
    [DllImport("shell32.dll")]
    private static extern int SHGetPropertyStoreForWindow(nint window, [MarshalAs(UnmanagedType.LPStruct)] Guid iid, out IPropertyStore store);
    [DllImport("ole32.dll")] private static extern int PropVariantClear(ref PropVariant value);
    [DllImport("gdi32.dll")] private static extern bool DeleteObject(nint handle);
    [DllImport("gdi32.dll")] private static extern int GetDIBits(nint dc, nint bitmap, uint start, int lines, byte[] bits, ref BitmapInfoHeader info, uint usage);
    [DllImport("user32.dll")] private static extern nint GetDC(nint window);
    [DllImport("user32.dll")] private static extern int ReleaseDC(nint window, nint dc);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern nint FindWindowEx(nint parent, nint after, string className, string? title);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern int GetApplicationUserModelId(nint process, ref int length, StringBuilder id);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowText(nint window, StringBuilder text, int count);

    private const uint ProcessQueryLimitedInformation = 0x1000;
    private const int WmGetIcon = 0x007F;
    private const int IconBig = 1;
    private const int GclpHIcon = -14;

    private delegate bool EnumWindowsProc(nint window, nint parameter);
    [DllImport("kernel32.dll")] private static extern nint OpenProcess(uint access, bool inherit, int processId);
    [DllImport("kernel32.dll")] private static extern bool CloseHandle(nint handle);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern bool QueryFullProcessImageName(nint process, uint flags, StringBuilder path, ref int size);
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern int SHDefExtractIcon(string iconFile, int index, uint flags, out nint large, out nint small, uint sizes);
    [DllImport("user32.dll")] private static extern bool DestroyIcon(nint icon);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindowsProc callback, nint parameter);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(nint window, out int processId);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(nint window);
    private const uint SmtoAbortIfHung = 0x0002;
    [DllImport("user32.dll")]
    private static extern nint SendMessageTimeout(nint window, int message, nint wParam, nint lParam, uint flags, uint timeout, out nint result);
    [DllImport("user32.dll", EntryPoint = "GetClassLongPtrW")] private static extern nint GetClassLongPtr(nint window, int index);
}
