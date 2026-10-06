# Dev tool for the experimental DXGI glass (hook/dxgiglass.cpp): finds areas of an app that must stay solid with UI
# Automation and publishes them every half second in Local\GlassForge.DxgiKeep.<pid>, where the hook reads them. It
# follows the app across restarts. By default that is LogMeIn Resolve's remote screen: the session page's <video>
# element (AutomationId ending in "VideoStream_Video", visible only in the raw view).
#
# LogMeIn's embedded Chromium stops reporting the inside of the session page after full screen is toggled (the page
# itself stays visible to UI Automation, its contents don't). So while the video element is visible, its position
# inside the session page is remembered as insets from the page's edges, and when the element is gone the same insets
# are applied to the page's current rectangle. Runs until the stop file appears.
param(
    [string] $Process = 'ra-technician-console',
    [string] $DocumentPrefix = 'session',
    [string] $AutomationIdSuffix = 'VideoStream_Video',
    [string] $StopFile = "$env:LOCALAPPDATA\GlassForge\hook.stop"
)
Add-Type -Namespace Ka -Name W -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr value);
[DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr window, ref POINT point);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out RECT rect);
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr window, out RECT rect);
public struct POINT { public int X, Y; }
public struct RECT { public int Left, Top, Right, Bottom; }
'@
[void][Ka.W]::SetProcessDpiAwarenessContext([IntPtr]-4) # per-monitor v2: UI Automation rectangles in physical pixels
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes

$walker = [Windows.Automation.TreeWalker]::RawViewWalker
$documents = New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::ControlTypeProperty, [Windows.Automation.ControlType]::Document)

function Find-Video($element, [int] $depth) {
    if ($depth -gt 40) { return $null }
    $child = $walker.GetFirstChild($element)
    while ($child) {
        if ($child.Current.AutomationId.EndsWith($AutomationIdSuffix)) { return $child }
        $found = Find-Video $child ($depth + 1)
        if ($found) { return $found }
        $child = $walker.GetNextSibling($child)
    }
    return $null
}

function Find-SessionPage($window) {
    $root = [Windows.Automation.AutomationElement]::FromHandle($window)
    foreach ($doc in $root.FindAll([Windows.Automation.TreeScope]::Descendants, $documents)) {
        if ($doc.Current.Name.StartsWith($DocumentPrefix)) { return $doc }
    }
    return $null
}

$last = ''
$targetId = 0
$mapping = $null
$view = $null
$lastFrame = ''
$insets = $null # remote screen's distance from the session page's left, top, right and bottom edges
# Found elements are reused and only their rectangles re-read; walking LogMeIn's page tree means many calls into its
# own threads, so a full search happens only every 3 s, when something is lost, or after the window settles.
$page = $null
$video = $null
$searched = [datetime]::MinValue
while (-not (Test-Path $StopFile)) {
    # Follow the app across restarts: a new process gets a new mapping (the name carries its pid).
    $target = Get-Process -Name $Process -ErrorAction SilentlyContinue | Where-Object MainWindowHandle -ne 0 | Select-Object -First 1
    if (-not $target) { Start-Sleep -Milliseconds 500; continue }
    if ($target.Id -ne $targetId) {
        if ($view) { $view.Dispose(); $mapping.Dispose() }
        $targetId = $target.Id
        $window = $target.MainWindowHandle
        $mapping = [IO.MemoryMappedFiles.MemoryMappedFile]::CreateOrOpen("Local\GlassForge.DxgiKeep.$targetId", 4 + 8 * 16)
        $view = $mapping.CreateViewAccessor()
        $last = ''
        $insets = $null
        Write-Output "$(Get-Date -Format HH:mm:ss) publishing keep areas for $Process (pid $targetId, window $window)"
    }
    # While the window moves or resizes, UI Automation and the window position are read at slightly different moments,
    # which gives shifted rectangles; keep what was published until it settles.
    $frame = New-Object Ka.W+RECT
    [void][Ka.W]::GetWindowRect($window, [ref]$frame)
    $frameText = "$($frame.Left),$($frame.Top),$($frame.Right),$($frame.Bottom)"
    if ($frameText -ne $lastFrame) { $lastFrame = $frameText; $searched = [datetime]::MinValue; Start-Sleep -Milliseconds 250; continue }

    $rects = @()
    $source = ''
    try {
        $origin = New-Object Ka.W+POINT
        [void][Ka.W]::ClientToScreen($window, [ref]$origin)
        $client = New-Object Ka.W+RECT
        [void][Ka.W]::GetClientRect($window, [ref]$client)
        if (-not $page -or ([datetime]::Now - $searched).TotalSeconds -ge 3) {
            $searched = [datetime]::Now
            $page = Find-SessionPage $window
            $video = if ($page) { Find-Video $page 0 } else { $null }
        }
        if ($page) {
            $p = $page.Current.BoundingRectangle
            $v = $null
            if ($video) { try { $v = $video.Current.BoundingRectangle } catch { $video = $null } }
            # A real video rectangle is non-empty, inside the window, and inside its page (after full screen ends, the
            # old element lingers for seconds with its full-screen size).
            $videoOk = $v -and -not $v.IsEmpty -and $v.Width -ge 1 -and $v.Width -le $client.Right + 8 -and $v.Height -le $client.Bottom + 8 `
                -and $v.Left -ge $p.Left - 2 -and $v.Top -ge $p.Top - 2 -and $v.Right -le $p.Right + 2 -and $v.Bottom -le $p.Bottom + 2
            if ($videoOk) {
                $insets = @(($v.Left - $p.Left), ($v.Top - $p.Top), ($p.Right - $v.Right), ($p.Bottom - $v.Bottom))
                $box = @($v.Left, $v.Top, $v.Right, $v.Bottom)
                $source = 'video'
            } elseif ($insets -and -not $p.IsEmpty) {
                $box = @(($p.Left + $insets[0]), ($p.Top + $insets[1]), ($p.Right - $insets[2]), ($p.Bottom - $insets[3]))
                $source = 'page+insets'
            } elseif (-not $p.IsEmpty) {
                $box = @($p.Left, $p.Top, $p.Right, $p.Bottom) # nothing better known yet: the whole session page stays solid
                $source = 'page'
            }
            if ($source) {
                $rects += , @([int]($box[0] - $origin.X), [int]($box[1] - $origin.Y), [int]($box[2] - $origin.X), [int]($box[3] - $origin.Y))
            }
        }
    } catch { $page = $null; $video = $null } # gone: search again next time
    for ($i = 0; $i -lt $rects.Count; $i++) { for ($j = 0; $j -lt 4; $j++) { $view.Write(4 + $i * 16 + $j * 4, [int]$rects[$i][$j]) } }
    $view.Write(0, [int]$rects.Count)
    $now = ($rects | ForEach-Object { "[" + ($_ -join ",") + "]" }) -join " "
    if ($now -ne $last) { Write-Output "$(Get-Date -Format HH:mm:ss) keep: $(if ($now) { "$now from $source" } else { 'none' })"; $last = $now }
    Start-Sleep -Milliseconds 500
}
if ($view) { $view.Dispose(); $mapping.Dispose() }
