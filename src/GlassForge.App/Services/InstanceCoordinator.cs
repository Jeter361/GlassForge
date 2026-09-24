using System.ComponentModel;
using System.Diagnostics;
using System.IO;

namespace GlassForge.Services;

/// <summary>
/// Keeps exactly one GlassForge running. Launching a copy from a different location (a newer download, a
/// rebuilt binary) replaces the running one; launching the same executable again is a no-op.
/// </summary>
internal sealed class InstanceCoordinator : IDisposable
{
    private const string MutexName = @"Local\GlassForge.SingleInstance";
    private const string ExitRequestName = @"Local\GlassForge.ExitRequest";

    private readonly Mutex _mutex;
    private EventWaitHandle? _exitRequest;

    internal enum Outcome { FirstInstance, ReplacedOlderCopy, AlreadyRunning, CouldNotReplace }

    private InstanceCoordinator(Mutex mutex) => _mutex = mutex;

    internal static (InstanceCoordinator? Coordinator, Outcome Outcome) Acquire()
    {
        var mutex = new Mutex(true, MutexName, out var first);
        if (first) return (new InstanceCoordinator(mutex), Outcome.FirstInstance);

        var others = FindOtherInstances();
        var self = Environment.ProcessPath;
        if (others.Count == 0 || others.Any(other => PathEquals(other.Path, self)))
        {
            mutex.Dispose();
            return (null, Outcome.AlreadyRunning);
        }

        // Ask the running copy to exit cleanly so it restores its windows. Versions before 0.1.3 don't listen
        // for the request; force-close them instead and let WindowStyleLedger restore what they left behind.
        if (EventWaitHandle.TryOpenExisting(ExitRequestName, out var request))
            using (request) request.Set();
        if (!TryTake(mutex, TimeSpan.FromSeconds(5)))
        {
            foreach (var other in others)
            {
                try { Process.GetProcessById(other.Id).Kill(); }
                catch (ArgumentException) { }
                catch (Win32Exception) { }
                catch (InvalidOperationException) { }
            }
            if (!TryTake(mutex, TimeSpan.FromSeconds(3)))
            {
                mutex.Dispose();
                return (null, Outcome.CouldNotReplace);
            }
        }
        return (new InstanceCoordinator(mutex), Outcome.ReplacedOlderCopy);
    }

    /// <summary>Invokes <paramref name="onExitRequested"/> when a newer copy asks this one to step aside.</summary>
    internal void ListenForExitRequests(Action onExitRequested)
    {
        _exitRequest = new EventWaitHandle(false, EventResetMode.AutoReset, ExitRequestName);
        var handle = _exitRequest;
        new Thread(() =>
        {
            try
            {
                handle.WaitOne();
                onExitRequested();
            }
            catch (ObjectDisposedException) { }
        }) { IsBackground = true, Name = "GlassForge:ExitRequest" }.Start();
    }

    public void Dispose()
    {
        _exitRequest?.Dispose();
        _mutex.Dispose();
    }

    private static bool TryTake(Mutex mutex, TimeSpan timeout)
    {
        try { return mutex.WaitOne(timeout); }
        catch (AbandonedMutexException) { return true; }
    }

    // Release downloads can be renamed ("GlassForge (1).exe"), so match on the process name prefix.
    private static List<(int Id, string? Path)> FindOtherInstances()
    {
        var results = new List<(int, string?)>();
        foreach (var process in Process.GetProcesses())
        {
            using (process)
            {
                if (process.Id == Environment.ProcessId || !process.ProcessName.StartsWith("GlassForge", StringComparison.OrdinalIgnoreCase)) continue;
                string? path = null;
                try { path = process.MainModule?.FileName; }
                catch (Win32Exception) { }
                catch (InvalidOperationException) { }
                results.Add((process.Id, path));
            }
        }
        return results;
    }

    private static bool PathEquals(string? left, string? right) =>
        left is not null && right is not null && string.Equals(Path.GetFullPath(left), Path.GetFullPath(right), StringComparison.OrdinalIgnoreCase);
}
