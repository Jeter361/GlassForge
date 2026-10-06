# Dev tool for the experimental DXGI glass on Store (MSIX) apps: with -On, every launch of the package starts paused
# and runs the hook's AttachToPackageProcess first (Windows' package debug settings, IPackageDebugSettings), so the hook
# is in the app from its first instruction - the Store-app equivalent of LaunchWithGlass. -Off restores normal launches.
# The app must also be listed in %LOCALAPPDATA%\GlassForge\dxgi.txt for the hook to switch its glass on.
param(
    [Parameter(Mandatory)] [string] $Package, # package family name prefix, e.g. OpenAI.ChatGPT-Desktop
    [switch] $On,
    [switch] $Off
)
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
[ComImport, Guid("F27C3930-8029-4AD1-94E3-3DBA417810C1"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IPackageDebugSettings
{
    [PreserveSig] int EnableDebugging([MarshalAs(UnmanagedType.LPWStr)] string packageFullName, [MarshalAs(UnmanagedType.LPWStr)] string debuggerCommandLine, IntPtr environment);
    [PreserveSig] int DisableDebugging([MarshalAs(UnmanagedType.LPWStr)] string packageFullName);
}
[ComImport, Guid("B1AEC16F-2383-4852-B0E9-8F0B1DC66B4D")] public class PackageDebugSettings { }
public static class PackageDebug
{
    public static int Enable(string package, string command) { return ((IPackageDebugSettings)new PackageDebugSettings()).EnableDebugging(package, command, IntPtr.Zero); }
    public static int Disable(string package) { return ((IPackageDebugSettings)new PackageDebugSettings()).DisableDebugging(package); }
}
'@
$full = (Get-AppxPackage -Name $Package).PackageFullName
if (-not $full) { throw "package $Package not found" }
if ($On) {
    $root = Split-Path $PSScriptRoot
    $dir = "$env:LOCALAPPDATA\GlassForge\hook"
    New-Item -ItemType Directory -Force $dir | Out-Null
    $dll = Join-Path $dir ("GlassForge.Hook.{0:yyyyMMddHHmmssfff}.dll" -f (Get-Date))
    Copy-Item "$root\build\Release\GlassForge.Hook.dll" $dll
    $command = "$env:WINDIR\System32\rundll32.exe `"$dll`",AttachToPackageProcess"
    $hr = [PackageDebug]::Enable($full, $command)
    "enable for $full -> 0x{0:X8}" -f $hr
}
if ($Off) {
    $hr = [PackageDebug]::Disable($full)
    "disable for $full -> 0x{0:X8}" -f $hr
}
