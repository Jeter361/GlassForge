# Dev loop for surveys that must see an app's startup: launches the exe, hooks every UI thread that shows a window
# during the first seconds (the splash screen included), and holds the hooks until the stop file appears.
param(
    [Parameter(Mandatory)] [string] $Exe,
    [string] $Arguments = '',
    [int] $Seconds = 20,
    [string] $StopFile = "$env:LOCALAPPDATA\GlassForge\hook.stop"
)
$root = Split-Path $PSScriptRoot
$dir = "$env:LOCALAPPDATA\GlassForge\hook"
New-Item -ItemType Directory -Force $dir | Out-Null
$dll = Join-Path $dir ("GlassForge.Hook.{0:yyyyMMddHHmmssfff}.dll" -f (Get-Date))
Copy-Item "$root\build\Release\GlassForge.Hook.dll" $dll
Add-Type -Namespace Lh -Name W -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern IntPtr LoadLibraryW(string path);
[DllImport("kernel32.dll", CharSet = CharSet.Ansi)] public static extern IntPtr GetProcAddress(IntPtr module, string name);
[DllImport("user32.dll", SetLastError = true)] public static extern IntPtr SetWindowsHookExW(int id, IntPtr proc, IntPtr module, uint threadId);
[DllImport("user32.dll")] public static extern bool UnhookWindowsHookEx(IntPtr hook);
[DllImport("user32.dll")] public static extern bool PostThreadMessageW(uint thread, uint message, IntPtr w, IntPtr l);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern uint RegisterWindowMessageW(string name);
public delegate bool EnumProc(IntPtr window, IntPtr data);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr data);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
public static System.Collections.Generic.List<uint> Threads(uint pid) {
    var threads = new System.Collections.Generic.List<uint>();
    EnumWindows((w, d) => { uint p; uint t = GetWindowThreadProcessId(w, out p); if (p == pid && !threads.Contains(t)) threads.Add(t); return true; }, IntPtr.Zero);
    return threads;
}
'@
$module = [Lh.W]::LoadLibraryW($dll)
$proc = [Lh.W]::GetProcAddress($module, 'GlassForgeGetMessageProc')
Remove-Item $StopFile -ErrorAction SilentlyContinue
$process = Start-Process $Exe -ArgumentList $Arguments -PassThru
$hooks = @{}
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline -and -not $process.HasExited) {
    foreach ($thread in [Lh.W]::Threads([uint32]$process.Id)) {
        if ($hooks.ContainsKey($thread)) { continue }
        $hooks[$thread] = [Lh.W]::SetWindowsHookExW(3, $proc, $module, $thread) # WH_GETMESSAGE
        [void][Lh.W]::PostThreadMessageW($thread, 0, [IntPtr]::Zero, [IntPtr]::Zero)
        Write-Output ("{0:HH:mm:ss.fff} hooked pid {1} thread {2}" -f (Get-Date), $process.Id, $thread)
    }
    Start-Sleep -Milliseconds 10
}
Write-Output "holding $($hooks.Count) hook(s) until $StopFile exists"
while (-not (Test-Path $StopFile) -and -not $process.HasExited) { Start-Sleep -Milliseconds 300 }
$detach = [Lh.W]::RegisterWindowMessageW('GlassForge.Hook.Detach')
foreach ($thread in $hooks.Keys) { [void][Lh.W]::PostThreadMessageW($thread, $detach, [IntPtr]::Zero, [IntPtr]::Zero) }
Start-Sleep -Milliseconds 700
foreach ($hook in $hooks.Values) { [void][Lh.W]::UnhookWindowsHookEx($hook) }
foreach ($thread in $hooks.Keys) { [void][Lh.W]::PostThreadMessageW($thread, 0, [IntPtr]::Zero, [IntPtr]::Zero) }
Remove-Item $StopFile -ErrorAction SilentlyContinue
