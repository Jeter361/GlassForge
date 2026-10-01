# Dev loop: injects the freshly built hook into one window, repaints it, optionally dumps the survey, and leaves the
# hook running until stop.ps1 (or the stop file) ends it. The DLL is copied to a unique name so rebuilding works
# while an old copy is still loaded somewhere.
param([Parameter(Mandatory)] [long] $Window, [switch] $Dump)
$root = Split-Path $PSScriptRoot
$dir = "$env:LOCALAPPDATA\GlassForge\hook"
New-Item -ItemType Directory -Force $dir | Out-Null
$dll = Join-Path $dir ("GlassForge.Hook.{0:yyyyMMddHHmmssfff}.dll" -f (Get-Date))
Copy-Item "$root\build\Release\GlassForge.Hook.dll" $dll
Add-Type -Namespace Rt -Name W -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr h, IntPtr r, IntPtr g, uint f);
'@
Start-Process powershell -WindowStyle Hidden -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$PSScriptRoot\inject.ps1", '-Window', $Window, '-Dll', $dll
Start-Sleep 2
# RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW
[void][Rt.W]::RedrawWindow([IntPtr]$Window, [IntPtr]::Zero, [IntPtr]::Zero, 0x0001 -bor 0x0004 -bor 0x0400 -bor 0x0080 -bor 0x0100)
Start-Sleep 1
if ($Dump) { & "$PSScriptRoot\dump.ps1" -Window $Window; Start-Sleep 1 }
