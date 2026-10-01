# Asks the hooked window to write its survey counts to %LOCALAPPDATA%\GlassForge\hook.log.
param([Parameter(Mandatory)] [long] $Window)
Add-Type -Namespace Dmp -Name W -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr window, uint message, IntPtr w, IntPtr l);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern uint RegisterWindowMessageW(string name);
'@
[void][Dmp.W]::PostMessageW([IntPtr]$Window, [Dmp.W]::RegisterWindowMessageW('GlassForge.Hook.Dump'), [IntPtr]::Zero, [IntPtr]::Zero)
