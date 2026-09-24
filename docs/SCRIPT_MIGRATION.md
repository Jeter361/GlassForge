# Relationship to the PowerShell prototypes

## Does GlassForge run the old scripts?

No. GlassForge does not launch `TeamsBlurHost.ps1`, `TeamsGlass.ps1`, or the application-specific VBS files.

The scripts were prototypes used to discover a working composition technique and tune its behavior interactively. Their logic was ported into compiled C# services inside GlassForge.

This distinction matters because the product should not depend on hidden PowerShell processes, execution-policy overrides, or one startup launcher per application.

## Prototype-to-product mapping

| Prototype behavior | GlassForge implementation |
|---|---|
| Enumerate windows owned by a process | `WindowDiscoveryService` |
| Find an app whose `MainWindowHandle` is zero | `EnumWindows` plus owning PID lookup |
| Create the backing acrylic window | `GlassHostForm` |
| Enable host-backdrop acrylic | `GlassHostForm.EnableAcrylic` |
| Apply foreground opacity | `GlassHostForm.Align` |
| Follow movement and resize | WinEvent callback plus tracking fallback |
| Restore focus Z-order immediately | Foreground WinEvent callback |
| Run one hidden process per app | One managed `GlassHostSession` per profile |
| VBS files in the Startup folder | `StartupService` and one GlassForge background process |
| Hard-coded Teams/Outlook/Word scripts | User-created process profiles |
| Manual recovery | Automatic style restoration plus reset utility |

## Status of files in `C:\glass`

### Historical prototypes

- `TeamsBlurHost.ps1`
- `TeamsGlass.ps1`
- `TeamsTransparency.ps1`
- `OutlookTransparency.ps1`
- Application-specific `*GlassHidden.vbs` files
- Other earlier transparency scripts

These remain useful as research history but are not runtime dependencies of GlassForge.

### Disabled prototype startup entries

The Teams, Outlook, and Word VBS startup files were renamed with a `.disabled` suffix. They should remain disabled while testing GlassForge to prevent duplicate hosts.

### Recovery tool

`C:\glass\ResetGlassWindows.ps1` resets layered opacity and backdrop policies for Teams, Outlook, and Word. It is intended for development recovery after a script or process is force-terminated.

## Why the product no longer uses scripts

- One process manages every profile.
- The UI can start, stop, and restart effects predictably.
- Original window styles can be tracked and restored.
- Native hooks have stable delegate lifetimes.
- Configuration is structured and persistent.
- Build-time warnings and smoke tests catch regressions.
- Distribution does not require users to understand PowerShell execution policy.

## Testing without prototype conflicts

Before starting GlassForge, confirm no prototype host remains:

```powershell
Get-CimInstance Win32_Process |
  Where-Object CommandLine -match 'C:\\glass\\.*(BlurHost|Glass|Transparency).*\.ps1'
```

The normal target state before a GlassForge test is:

- No matching PowerShell host process.
- Legacy `.vbs` startup entries disabled.
- Teams, Outlook, and Word opaque until GlassForge applies a profile.

