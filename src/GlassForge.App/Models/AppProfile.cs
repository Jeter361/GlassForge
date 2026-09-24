using System.ComponentModel;
using System.IO;
using System.Runtime.CompilerServices;
using System.Text.Json.Serialization;

namespace GlassForge.Models;

public sealed class AppProfile : INotifyPropertyChanged
{
    private string _displayName = "New profile";
    private string _processName = "";
    private bool _enabled = true;
    private byte _windowOpacity = 195;
    private byte _tintOpacity = 32;
    private string _tintColor = "181818";

    public Guid Id { get; set; } = Guid.NewGuid();
    public string DisplayName { get => _displayName; set => Set(ref _displayName, value); }
    public string ProcessName { get => _processName; set => Set(ref _processName, Path.GetFileNameWithoutExtension(value.Trim())); }
    public bool Enabled { get => _enabled; set => Set(ref _enabled, value); }
    public byte WindowOpacity { get => _windowOpacity; set => Set(ref _windowOpacity, value); }
    public byte TintOpacity { get => _tintOpacity; set => Set(ref _tintOpacity, value); }
    public string TintColor { get => _tintColor; set => Set(ref _tintColor, value.Trim().TrimStart('#').ToUpperInvariant()); }

    [JsonIgnore] public string OpacityLabel => $"{Math.Round(WindowOpacity / 255d * 100)}%";
    [JsonIgnore] public string TintLabel => $"{Math.Round(TintOpacity / 255d * 100)}%";
    public event PropertyChangedEventHandler? PropertyChanged;

    private void Set<T>(ref T field, T value, [CallerMemberName] string? property = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return;
        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(property));
        if (property == nameof(WindowOpacity)) PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(OpacityLabel)));
        if (property == nameof(TintOpacity)) PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(TintLabel)));
    }
}
