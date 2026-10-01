using System.Globalization;
using System.Windows.Data;

namespace GlassForge.Converters;

/// <summary>Binds a RadioButton's IsChecked to one enum value: ConverterParameter names the value it stands for.</summary>
public sealed class EnumMatchConverter : IValueConverter
{
    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture) =>
        value is not null && string.Equals(value.ToString(), parameter as string, StringComparison.Ordinal);

    public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture) =>
        value is true && parameter is string name ? Enum.Parse(targetType, name) : System.Windows.Data.Binding.DoNothing;
}
