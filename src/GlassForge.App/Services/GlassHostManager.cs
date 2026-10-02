using GlassForge.Models;

namespace GlassForge.Services;

internal interface IGlassSession : IDisposable
{
    void Start();
}

internal sealed class CompositeSession(params IGlassSession[] sessions) : IGlassSession
{
    public void Start() { foreach (var session in sessions) session.Start(); }

    // Stop in reverse so the host gives back the app's opacity before the native session restores its frame.
    public void Dispose() { foreach (var session in sessions.Reverse()) session.Dispose(); }
}

public sealed class GlassHostManager : IDisposable
{
    private readonly Dictionary<Guid, IGlassSession> _sessions = [];

    public bool IsRunning(Guid profileId) => _sessions.ContainsKey(profileId);

    public void Start(AppProfile profile)
    {
        Stop(profile.Id);
        if (string.IsNullOrWhiteSpace(profile.ProcessName)) return;
        IGlassSession session = profile.Mode switch
        {
            // Stops in reverse: the hook gives each window its own frame back before the native session restores
            // what it set.
            // Office apps (Word, Excel, PowerPoint) only get the hook: Office skips its own Mica (and so the brush the
            // hook swaps) when the window already has a DWM backdrop, which the native session would set.
            GlassMode.Native when profile.IsOffice && profile.HookGlass && HookLibrary.IsAvailable =>
                new ExplorerHookSession(AppProfile.AnyWindowClass, profile.ProcessName),
            GlassMode.Native when profile.HookGlass && profile.HookWindowClass is { } windowClass && HookLibrary.IsAvailable =>
                new CompositeSession(new NativeBackdropSession(profile), new ExplorerHookSession(windowClass, profile.ProcessName)),
            GlassMode.Native => new NativeBackdropSession(profile),
            _ => new GlassHostSession(profile)
        };
        _sessions[profile.Id] = session;
        session.Start();
    }

    public void Stop(Guid profileId)
    {
        if (!_sessions.Remove(profileId, out var session)) return;
        session.Dispose();
    }

    public void Restart(AppProfile profile)
    {
        if (profile.Enabled) Start(profile); else Stop(profile.Id);
    }

    public void Dispose()
    {
        foreach (var session in _sessions.Values) session.Dispose();
        _sessions.Clear();
    }
}

