using System.Text.Json;
using System.IO;
using GlassForge.Models;

namespace GlassForge.Services;

public sealed class ProfileStore
{
    private static readonly JsonSerializerOptions Options = new() { WriteIndented = true };
    public string FilePath { get; } = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "GlassForge", "profiles.json");

    public IReadOnlyList<AppProfile> Load()
    {
        try
        {
            if (File.Exists(FilePath))
                return JsonSerializer.Deserialize<List<AppProfile>>(File.ReadAllText(FilePath), Options) ?? [];
        }
        catch (JsonException) { }
        catch (IOException) { }

        return
        [
            New("Microsoft Teams", "ms-teams"),
            New("Microsoft Outlook", "olk"),
            New("Microsoft Word", "WINWORD")
        ];
    }

    public void Save(IEnumerable<AppProfile> profiles)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
        var temporary = FilePath + ".tmp";
        File.WriteAllText(temporary, JsonSerializer.Serialize(profiles, Options));
        File.Move(temporary, FilePath, true);
    }

    private static AppProfile New(string name, string process) => new() { DisplayName = name, ProcessName = process };
}
