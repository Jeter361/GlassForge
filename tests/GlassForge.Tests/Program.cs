using GlassForge.Models;
using GlassForge.Services;

var failures = new List<string>();

var profile = new AppProfile { ProcessName = "C:\\Program Files\\Example\\sample.exe", TintColor = "#a1b2c3" };
Check(profile.ProcessName == "sample", "Process names normalize from executable paths.");
Check(profile.TintColor == "A1B2C3", "Tint colors normalize to six-digit uppercase hex.");
Check(profile.WindowOpacity == 195, "Profiles use the proven 76% default window opacity.");
Check(profile.TintOpacity == 32, "Profiles use the neutral low-strength tint default.");
Check(!profile.PauseWhileMediaPlays, "Media pause is opt-in so music players keep their glass.");

bool MediaMatch(string id, string process) => MediaPlaybackMonitor.Matches(id, MediaPlaybackMonitor.Normalize(process));
Check(MediaMatch("Chrome", "chrome"), "Chrome media sessions match chrome.exe.");
Check(MediaMatch("Chrome.UserData.Default", "chrome"), "Profile-specific Chrome session IDs match.");
Check(MediaMatch("MSEdge", "msedge") && MediaMatch("MSEdgeBeta", "msedge"), "Edge channels match msedge.exe.");
Check(MediaMatch("Spotify.exe", "Spotify"), "Executable-name session IDs match.");
Check(MediaMatch("MSTeams_8wekyb3d8bbwe!MSTeams", "ms-teams"), "Packaged app IDs match hyphenated process names.");
Check(!MediaMatch("Spotify.exe", "chrome"), "Other apps' sessions do not match.");

if (failures.Count > 0)
{
    foreach (var failure in failures) Console.Error.WriteLine($"FAIL: {failure}");
    return 1;
}

Console.WriteLine("All GlassForge smoke tests passed.");
return 0;

void Check(bool condition, string message)
{
    if (!condition) failures.Add(message);
}

