using GlassForge.Models;

namespace GlassForge.Services;

public sealed class GlassHostManager : IDisposable
{
    private readonly Dictionary<Guid, GlassHostSession> _sessions = [];

    public bool IsRunning(Guid profileId) => _sessions.ContainsKey(profileId);

    public void Start(AppProfile profile)
    {
        Stop(profile.Id);
        if (string.IsNullOrWhiteSpace(profile.ProcessName)) return;
        var session = new GlassHostSession(profile);
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

