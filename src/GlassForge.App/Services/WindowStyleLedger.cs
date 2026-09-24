using System.IO;
using System.Text.Json;
using GlassForge.Native;

namespace GlassForge.Services;

/// <summary>
/// Persists the original extended style of every target window GlassForge makes translucent,
/// so a later run can restore windows left behind by a crash or a killed process.
/// </summary>
internal static class WindowStyleLedger
{
    private sealed record Entry(long Window, uint ProcessId, long OriginalStyle);

    private static readonly object Gate = new();
    private static readonly string FilePath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "GlassForge", "touched-windows.json");

    internal static void Record(nint window, nint originalStyle)
    {
        NativeMethods.GetWindowThreadProcessId(window, out var processId);
        lock (Gate)
        {
            var entries = Load();
            entries.RemoveAll(entry => entry.Window == window);
            entries.Add(new Entry(window, processId, originalStyle));
            Save(entries);
        }
    }

    internal static void Forget(nint window)
    {
        lock (Gate)
        {
            var entries = Load();
            if (entries.RemoveAll(entry => entry.Window == window) > 0) Save(entries);
        }
    }

    /// <summary>Restores windows still recorded from a previous run. Call before any host starts.</summary>
    internal static int RestoreOrphans()
    {
        lock (Gate)
        {
            var restored = 0;
            foreach (var entry in Load())
            {
                var window = new nint(entry.Window);
                // Window handles are recycled; only touch the window if it still belongs to the same process.
                if (!NativeMethods.IsWindow(window)) continue;
                NativeMethods.GetWindowThreadProcessId(window, out var processId);
                if (processId != entry.ProcessId) continue;
                NativeMethods.SetLayeredWindowAttributes(window, 0, 255, NativeMethods.LwaAlpha);
                NativeMethods.SetWindowLongPtr(window, NativeMethods.GwlExStyle, new nint(entry.OriginalStyle));
                restored++;
            }
            Save([]);
            return restored;
        }
    }

    private static List<Entry> Load()
    {
        try
        {
            if (File.Exists(FilePath)) return JsonSerializer.Deserialize<List<Entry>>(File.ReadAllText(FilePath)) ?? [];
        }
        catch (JsonException) { }
        catch (IOException) { }
        return [];
    }

    private static void Save(List<Entry> entries)
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
            var temporary = FilePath + ".tmp";
            File.WriteAllText(temporary, JsonSerializer.Serialize(entries));
            File.Move(temporary, FilePath, true);
        }
        catch (IOException) { }
    }
}
