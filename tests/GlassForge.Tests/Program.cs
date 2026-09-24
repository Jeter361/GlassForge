using GlassForge.Models;

var failures = new List<string>();

var profile = new AppProfile { ProcessName = "C:\\Program Files\\Example\\sample.exe", TintColor = "#a1b2c3" };
Check(profile.ProcessName == "sample", "Process names normalize from executable paths.");
Check(profile.TintColor == "A1B2C3", "Tint colors normalize to six-digit uppercase hex.");
Check(profile.WindowOpacity == 195, "Profiles use the proven 76% default window opacity.");
Check(profile.TintOpacity == 32, "Profiles use the neutral low-strength tint default.");

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

