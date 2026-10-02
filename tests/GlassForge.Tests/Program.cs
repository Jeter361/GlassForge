using GlassForge.Models;
using GlassForge.Services;

var failures = new List<string>();

var profile = new AppProfile { ProcessName = "C:\\Program Files\\Example\\sample.exe", TintColor = "#a1b2c3" };
Check(profile.ProcessName == "sample", "Process names normalize from executable paths.");
Check(profile.TintColor == "A1B2C3", "Tint colors normalize to six-digit uppercase hex.");
Check(profile.WindowOpacity == 195, "Profiles use the proven 76% default window opacity.");
Check(profile.TintOpacity == 32, "Profiles use the neutral low-strength tint default.");
Check(!profile.PauseWhileMediaPlays, "Media pause is opt-in so music players keep their glass.");

Check(profile.Mode == GlassMode.Host, "Profiles default to the glass host, which works with every app.");
Check(profile.Corners == CornerStyle.Default && profile.TitleBar == TitleBarTheme.Default, "Window options leave the app alone by default.");

var styled = new AppProfile { Mode = GlassMode.Native, ExtendIntoClient = true, Corners = CornerStyle.Square, TitleBar = TitleBarTheme.Dark };
var notified = false;
styled.PropertyChanged += (_, _) => notified = true;
var copy = styled.Clone();
copy.DisplayName = "Changed";
Check(!notified, "Clones don't raise change notifications on the original's subscribers.");
Check(copy.Mode == GlassMode.Native && copy.ExtendIntoClient && copy.Corners == CornerStyle.Square
    && copy.TitleBar == TitleBarTheme.Dark && copy.Id == styled.Id, "Clones carry every setting.");

var json = System.Text.Json.JsonSerializer.Serialize(styled);
Check(json.Contains("\"Mode\":\"Native\"") && json.Contains("\"Corners\":\"Square\""), "Enums save as readable names.");
var oldMica = System.Text.Json.JsonSerializer.Deserialize<AppProfile>("{\"ProcessName\":\"olk\",\"Mode\":\"Native\",\"Backdrop\":\"Mica\"}")!;
Check(oldMica.Mode == GlassMode.Native, "Profiles saved with the removed Mica options still load.");
var legacy = System.Text.Json.JsonSerializer.Deserialize<AppProfile>("{\"DisplayName\":\"Old\",\"ProcessName\":\"olk\",\"WindowOpacity\":200}")!;
Check(legacy.Mode == GlassMode.Host && legacy.WindowOpacity == 200, "Profiles saved by older versions load as glass host profiles.");
Check(profile.KeepGlassWhenUnfocused && legacy.KeepGlassWhenUnfocused, "Native glass stays live when unfocused by default, including for older profiles.");

var converter = new GlassForge.Converters.EnumMatchConverter();
Check(converter.Convert(GlassMode.Native, typeof(bool), "Native", null!) is true, "Segment buttons check for their own value.");
Check(converter.Convert(GlassMode.Host, typeof(bool), "Native", null!) is false, "Segment buttons stay clear for other values.");
Check(converter.ConvertBack(true, typeof(CornerStyle), "RoundSmall", null!) is CornerStyle.RoundSmall, "Checking a segment selects its value.");
Check(TargetWindowStyler.CornerValue(CornerStyle.Square) == 1 && TargetWindowStyler.CornerValue(CornerStyle.Default) == 0, "Corner styles map to DWM values.");

var notepad = new AppProfile { ProcessName = "Notepad", Mode = GlassMode.Native };
notepad.ResetAppearance();
Check(notepad.DrawsOwnTitleBar && notepad.ExtendIntoClient && !new AppProfile { ProcessName = "olk" }.DrawsOwnTitleBar, "Notepad defaults to whole-window glass; other apps do not.");

var terminal = new AppProfile { ProcessName = "WindowsTerminal", HookGlass = true };
Check(terminal.HookWindowClass == "CASCADIA_HOSTING_WINDOW_CLASS" && new AppProfile { ProcessName = "explorer" }.HookWindowClass == "CabinetWClass" && new AppProfile { ProcessName = "olk" }.HookWindowClass is null, "Only Explorer and Terminal have a hook mode.");
Check(System.Text.Json.JsonSerializer.Serialize(terminal).Contains("\"ExplorerGlass\":true"), "The hook option keeps its saved name, so existing Explorer profiles still load it.");

Check(new AppProfile { ProcessName = "SystemSettings" }.HookWindowClass == AppProfile.UwpContentWindowClass, "Settings hooks its UWP content window.");
Check(new AppProfile { ProcessName = "WINWORD" }.HookWindowClass == AppProfile.AnyWindowClass, "Word hooks every window of its process (the splash screen included).");

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

