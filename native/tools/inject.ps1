# Dev injector: hooks GlassForge.Hook.dll into the thread that owns one window, and holds the hook until
# the stop file appears (or this process ends). Windows unloads the DLL from the target when the hook is removed.
param(
    [Parameter(Mandatory)] [long] $Window,
    [Parameter(Mandatory)] [string] $Dll,
    [string] $StopFile = "$env:LOCALAPPDATA\GlassForge\hook.stop"
)
Add-Type -Namespace Inj -Name W -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern IntPtr LoadLibraryW(string path);
[DllImport("kernel32.dll", CharSet = CharSet.Ansi)] public static extern IntPtr GetProcAddress(IntPtr module, string name);
[DllImport("user32.dll", SetLastError = true)] public static extern IntPtr SetWindowsHookExW(int id, IntPtr proc, IntPtr module, uint threadId);
[DllImport("user32.dll")] public static extern bool UnhookWindowsHookEx(IntPtr hook);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr window, uint message, IntPtr w, IntPtr l);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern uint RegisterWindowMessageW(string name);
'@
$hwnd = [IntPtr]$Window
$pid0 = 0
$tid = [Inj.W]::GetWindowThreadProcessId($hwnd, [ref]$pid0)
if ($tid -eq 0) { throw "window $Window not found" }
$module = [Inj.W]::LoadLibraryW($Dll)
if ($module -eq [IntPtr]::Zero) { throw "LoadLibrary failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
$proc = [Inj.W]::GetProcAddress($module, 'GlassForgeGetMessageProc')
$hook = [Inj.W]::SetWindowsHookExW(3, $proc, $module, $tid) # WH_GETMESSAGE
if ($hook -eq [IntPtr]::Zero) { throw "SetWindowsHookEx failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
[void][Inj.W]::PostMessageW($hwnd, 0, [IntPtr]::Zero, [IntPtr]::Zero) # WM_NULL makes the thread pick up the hook now
Remove-Item $StopFile -ErrorAction SilentlyContinue
Write-Output "hooked pid $pid0 thread $tid"
while (-not (Test-Path $StopFile)) { Start-Sleep -Milliseconds 300 }
# Take the detours out on the target's own thread before Windows unloads the DLL.
[void][Inj.W]::PostMessageW($hwnd, [Inj.W]::RegisterWindowMessageW('GlassForge.Hook.Detach'), [IntPtr]::Zero, [IntPtr]::Zero)
Start-Sleep -Milliseconds 700
[void][Inj.W]::UnhookWindowsHookEx($hook)
[void][Inj.W]::PostMessageW($hwnd, 0, [IntPtr]::Zero, [IntPtr]::Zero)
Remove-Item $StopFile -ErrorAction SilentlyContinue
