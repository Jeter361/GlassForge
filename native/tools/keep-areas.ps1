# Dev tool for the experimental DXGI glass (hook/dxgiglass.cpp): finds areas of an app that must stay solid with UI
# Automation and publishes them every half second in Local\GlassForge.DxgiKeep.<pid>, where the hook reads them. By default
# that is LogMeIn Resolve's remote screen: the session page's <video> element (AutomationId ending in
# "VideoStream_Video"), which only shows up in the raw view. Runs until the stop file appears.
param(
    [string] $Process = 'ra-technician-console',
    [string] $AutomationIdSuffix = 'VideoStream_Video',
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
$walker = [Windows.Automation.TreeWalker]::RawViewWalker

# Depth-first through the raw view; the remote screen is ~25 levels down inside the web console.
function Find-Element($element, [int] $depth) {
    if ($depth -gt 40) { return $null }
    $child = $walker.GetFirstChild($element)
    while ($child) {
        if ($child.Current.AutomationId.EndsWith($AutomationIdSuffix)) { return $child }
        $found = Find-Element $child ($depth + 1)
        if ($found) { return $found }
        $child = $walker.GetNextSibling($child)
    }
    return $null
}

Write-Output "publishing keep areas for $Process (pid $($target.Id), window $window)"
$last = ''
$element = $null
$searched = [datetime]::MinValue
while (-not (Test-Path $StopFile)) {
    $rects = @()
    try {
        # Reuse the element while it lives; look again every half second when there is none, so a new session is covered fast.
        if (-not $element -and ([datetime]::Now - $searched).TotalSeconds -ge 0.5) {
            $searched = [datetime]::Now
            $element = Find-Element ([Windows.Automation.AutomationElement]::FromHandle($window)) 0
        }
        if ($element) {
            $r = $element.Current.BoundingRectangle
            if (-not $r.IsEmpty -and $r.Width -ge 1) {
                $origin = New-Object Ka.W+POINT
                [void][Ka.W]::ClientToScreen($window, [ref]$origin)
                $rects += , @([int]($r.Left - $origin.X), [int]($r.Top - $origin.Y), [int]($r.Right - $origin.X), [int]($r.Bottom - $origin.Y))
            }
        }
    } catch { $element = $null } # the session closed: the element is gone
    for ($i = 0; $i -lt $rects.Count; $i++) { for ($j = 0; $j -lt 4; $j++) { $view.Write(4 + $i * 16 + $j * 4, [int]$rects[$i][$j]) } }
    $view.Write(0, [int]$rects.Count)
    $now = ($rects | ForEach-Object { "[" + ($_ -join ",") + "]" }) -join " "
    if ($now -ne $last) { Write-Output "$(Get-Date -Format HH:mm:ss) keep: $(if ($now) { $now } else { 'none' })"; $last = $now }
    Start-Sleep -Milliseconds 500
}
$view.Dispose(); $mapping.Dispose()
