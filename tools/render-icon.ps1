param([string]$OutDir = $PSScriptRoot)
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase
$ErrorActionPreference = 'Stop'

function Color([string]$hex) { [Windows.Media.ColorConverter]::ConvertFromString($hex) }
function Gradient([string]$from, [string]$to) {
    $g = New-Object Windows.Media.LinearGradientBrush
    $g.StartPoint = New-Object Windows.Point 0, 0; $g.EndPoint = New-Object Windows.Point 1, 1
    $g.GradientStops.Add((New-Object Windows.Media.GradientStop (Color $from), 0))
    $g.GradientStops.Add((New-Object Windows.Media.GradientStop (Color $to), 1))
    $g
}

# Drawn on a 256-unit canvas, matching the title-bar logo: a violet tile with a frosted-glass tile over it.
# Strokes are sized in output pixels so small icons keep a crisp 1px edge; the shadow only appears at 48px+.
function Render-Icon([int]$size) {
    $px = 256.0 / $size
    $canvas = New-Object Windows.Controls.Canvas; $canvas.Width = 256; $canvas.Height = 256

    $back = New-Object Windows.Shapes.Rectangle
    $back.Width = 164; $back.Height = 164; $back.RadiusX = 46; $back.RadiusY = 46
    $back.Fill = Gradient '#B79CFF' '#5B21B6'
    [Windows.Controls.Canvas]::SetLeft($back, 16); [Windows.Controls.Canvas]::SetTop($back, 16)

    $front = New-Object Windows.Shapes.Rectangle
    $front.Width = 164; $front.Height = 164; $front.RadiusX = 46; $front.RadiusY = 46
    # Lavender-tinted and mostly opaque so the glass tile reads on both dark and light taskbars,
    # while still letting the violet tile tint the overlap.
    $front.Fill = Gradient '#EBF8F6FF' '#D9DDD6FE'
    $front.Stroke = if ($size -ge 48) { Gradient '#FFFFFFFF' '#FFC4B5FD' } else { Gradient '#FFC4B5FD' '#FF8B5CF6' }
    $front.StrokeThickness = [Math]::Max(5, $px * 1.0)
    if ($size -ge 48) {
        $shadow = New-Object Windows.Media.Effects.DropShadowEffect
        $shadow.Color = Color '#4C1D95'; $shadow.Opacity = 0.45; $shadow.BlurRadius = 18; $shadow.ShadowDepth = 5; $shadow.Direction = 300
        $front.Effect = $shadow
    }
    [Windows.Controls.Canvas]::SetLeft($front, 76); [Windows.Controls.Canvas]::SetTop($front, 76)

    # A soft sheen across the top of the glass tile.
    $sheen = New-Object Windows.Shapes.Rectangle
    $sheen.Width = 164; $sheen.Height = 70; $sheen.RadiusX = 46; $sheen.RadiusY = 46
    $sheen.Fill = Gradient '#70FFFFFF' '#00FFFFFF'
    [Windows.Controls.Canvas]::SetLeft($sheen, 76); [Windows.Controls.Canvas]::SetTop($sheen, 76)

    [void]$canvas.Children.Add($back); [void]$canvas.Children.Add($front)
    if ($size -ge 32) { [void]$canvas.Children.Add($sheen) }

    $view = New-Object Windows.Controls.Viewbox
    $view.Width = $size; $view.Height = $size; $view.Child = $canvas
    $view.Measure((New-Object Windows.Size $size, $size)); $view.Arrange((New-Object Windows.Rect 0, 0, $size, $size)); $view.UpdateLayout()
    $bmp = New-Object Windows.Media.Imaging.RenderTargetBitmap $size, $size, 96, 96, ([Windows.Media.PixelFormats]::Pbgra32)
    $bmp.Render($view)
    $enc = New-Object Windows.Media.Imaging.PngBitmapEncoder
    $enc.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($bmp))
    $ms = New-Object IO.MemoryStream; $enc.Save($ms); , $ms.ToArray()
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 96, 128, 256
$pngs = @{}
foreach ($s in $sizes) { $pngs[$s] = Render-Icon $s; [IO.File]::WriteAllBytes((Join-Path $OutDir "icon-$s.png"), $pngs[$s]) }

# ICO container with PNG-compressed entries (supported since Windows Vista).
$ico = New-Object IO.MemoryStream; $w = New-Object IO.BinaryWriter $ico
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
foreach ($s in $sizes) {
    $data = $pngs[$s]; $dim = if ($s -ge 256) { 0 } else { $s }
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32); $w.Write([UInt32]$data.Length); $w.Write([UInt32]$offset)
    $offset += $data.Length
}
foreach ($s in $sizes) { $w.Write($pngs[$s]) }
$w.Flush(); [IO.File]::WriteAllBytes((Join-Path $OutDir 'GlassForge.ico'), $ico.ToArray())
"wrote GlassForge.ico ($([Math]::Round($ico.Length / 1KB, 1)) KB, $($sizes.Count) sizes)"
