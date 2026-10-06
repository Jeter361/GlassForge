# Dev tool for the experimental DXGI glass (hook/dxgiglass.cpp): finds areas of an app that must stay solid (by
# default LogMeIn Resolve's remote-session view, an embedded page named "session/...") with UI Automation, and publishes
# them every second in Local\GlassForge.DxgiKeep.<pid>, where the hook reads them. Runs until the stop file appears.
param(
    [string] $Process = 'ra-technician-console',
    [string] $DocumentPrefix = 'session',
    [string] $StopFile = "$env:LOCALAPPDATA\GlassForge\hook.stop"
)
Add-Type -Namespace Ka -Name W -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr value);
[DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr window, ref POINT point);
public struct POINT { public int X, Y; }
'@
[void][Ka.W]::SetProcessDpiAwarenessContext([IntPtr]-4) # per-monitor v2: UI Automation rectangles in physical pixels
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes

$target = Get-Process -Name $Process | Where-Object MainWindowHandle -ne 0 | Select-Object -First 1
if (-not $target) { throw "$Process has no window" }
$window = $target.MainWindowHandle
$mapping = [IO.MemoryMappedFiles.MemoryMappedFile]::CreateOrOpen("Local\GlassForge.DxgiKeep.$($target.Id)", 4 + 8 * 16)
$view = $mapping.CreateViewAccessor()
$documents = New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::ControlTypeProperty, [Windows.Automation.ControlType]::Document)
Write-Output "publishing keep areas for $Process (pid $($target.Id), window $window)"
$last = ''
while (-not (Test-Path $StopFile)) {
    $rects = @()
    try {
        $root = [Windows.Automation.AutomationElement]::FromHandle($window)
        $origin = New-Object Ka.W+POINT
        [void][Ka.W]::ClientToScreen($window, [ref]$origin)
        foreach ($doc in $root.FindAll([Windows.Automation.TreeScope]::Descendants, $documents)) {
            if (-not $doc.Current.Name.StartsWith($DocumentPrefix)) { continue }
            $r = $doc.Current.BoundingRectangle
            if ($r.IsEmpty -or $r.Width -lt 1) { continue }
            $rects += , @([int]($r.Left - $origin.X), [int]($r.Top - $origin.Y), [int]($r.Right - $origin.X), [int]($r.Bottom - $origin.Y))
        }
    } catch { }
    if ($rects.Count -gt 8) { $rects = $rects[0..7] }
    for ($i = 0; $i -lt $rects.Count; $i++) { for ($j = 0; $j -lt 4; $j++) { $view.Write(4 + $i * 16 + $j * 4, [int]$rects[$i][$j]) } }
    $view.Write(0, [int]$rects.Count)
    $now = ($rects | ForEach-Object { "[" + ($_ -join ",") + "]" }) -join " "
    if ($now -ne $last) { Write-Output "$(Get-Date -Format HH:mm:ss) keep: $(if ($now) { $now } else { 'none' })"; $last = $now }
    Start-Sleep -Milliseconds 1000
}
$view.Dispose(); $mapping.Dispose()
