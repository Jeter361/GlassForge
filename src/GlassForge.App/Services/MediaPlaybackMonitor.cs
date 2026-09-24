using Windows.Media.Control;

namespace GlassForge.Services;

/// <summary>
/// Reports whether an app is playing media, using the same Windows media sessions
/// that drive the volume flyout's now-playing controls.
/// </summary>
internal static class MediaPlaybackMonitor
{
    private static readonly Lazy<GlobalSystemMediaTransportControlsSessionManager?> Manager = new(() =>
    {
        try { return Task.Run(async () => await GlobalSystemMediaTransportControlsSessionManager.RequestAsync()).Result; }
        catch (Exception) { return null; }
    });

    internal static bool IsPlaying(string processName)
    {
        var target = Normalize(processName);
        if (target.Length == 0 || Manager.Value is not { } manager) return false;
        try
        {
            foreach (var session in manager.GetSessions())
            {
                if (!Matches(session.SourceAppUserModelId, target)) continue;
                if (session.GetPlaybackInfo()?.PlaybackStatus == GlobalSystemMediaTransportControlsSessionPlaybackStatus.Playing) return true;
            }
        }
        catch (Exception) { }
        return false;
    }

    // Session IDs look like "Chrome", "MSEdge", "Spotify.exe", or "MSTeams_8wekyb3d8bbwe!MSTeams".
    internal static bool Matches(string appUserModelId, string normalizedProcessName)
    {
        var end = appUserModelId.IndexOfAny(['!', '_', '.']);
        var id = Normalize(end < 0 ? appUserModelId : appUserModelId[..end]);
        return id.Length > 0 && id.StartsWith(normalizedProcessName, StringComparison.Ordinal);
    }

    internal static string Normalize(string value) =>
        new(value.Where(char.IsLetterOrDigit).Select(char.ToLowerInvariant).ToArray());
}
