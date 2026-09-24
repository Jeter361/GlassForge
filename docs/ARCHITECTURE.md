# GlassForge architecture

## System overview

```text
MainWindow / tray
       |
       +-- ProfileStore --------> profiles.json
       +-- StartupService ------> HKCU Run
       +-- WindowDiscovery -----> EnumWindows + process ownership
       |
       +-- GlassHostManager
               |
               +-- GlassHostSession (one per enabled profile)
                       |
                       +-- dedicated STA thread
                       +-- GlassHostForm (acrylic HWND)
                       +-- WinEvent movement/focus hooks
                       +-- DWM/Win32 composition calls
```

## Project structure

```text
GlassForge/
  src/GlassForge.App/
    App.xaml(.cs)              Application lifecycle and background mode
    MainWindow.xaml(.cs)       Profile editor, app discovery UI, and tray
    Models/
      AppProfile.cs            Serializable effect configuration
      RunningApplication.cs    Discovery result shown in the UI
    Native/
      NativeMethods.cs         P/Invoke declarations and Win32 constants
    Services/
      GlassHostManager.cs      Starts, restarts, and stops sessions
      GlassHostSession.cs      Core acrylic host and tracking engine
      ProfileStore.cs          Atomic JSON persistence
      StartupService.cs        Current-user Windows startup registration
      WindowDiscoveryService.cs Process-owned top-level window discovery
  tests/GlassForge.Tests/      Dependency-free smoke tests
  docs/                        User and developer documentation
```

## Startup lifecycle

1. `App.OnStartup` creates `ProfileStore` and `GlassHostManager`.
2. Stored profiles are loaded from local application data.
3. Each enabled profile starts unless `--no-effects` was supplied.
4. The main window opens unless `--background` was supplied.
5. Closing the main window hides it; explicit tray **Exit** shuts down sessions.
6. Each session restores window styles during disposal.

## Profile lifecycle

An `AppProfile` contains:

- Stable profile ID.
- User-facing name.
- Executable process name without `.exe`.
- Enabled state.
- Target-window opacity.
- Acrylic tint opacity.
- Acrylic RGB tint color.

`ProfileStore` writes all profiles to a temporary JSON file and atomically replaces the previous file. This avoids a partially written configuration if the process is interrupted.

## Window discovery

GlassForge calls `EnumWindows`, obtains each HWND's owning PID with `GetWindowThreadProcessId`, and compares it to processes returned by `Process.GetProcessesByName`.

It deliberately does not use window-title matching. Title matching can affect unrelated applications when a document or tab happens to contain the same words.

The current backend selects the largest visible top-level window for each process profile. Supporting every window owned by a process is a planned extension.

## Acrylic host lifecycle

Every enabled profile receives its own background STA thread and WinForms message loop. The session creates a borderless, non-activating, taskbar-hidden host window.

The host enables:

- `DWMWA_USE_HOSTBACKDROPBRUSH`
- Acrylic through `SetWindowCompositionAttribute`
- Rounded Windows 11 corners
- A tint encoded in the `AABBGGRR` format expected by the accent API

The target application receives `WS_EX_LAYERED` and a configured alpha through `SetLayeredWindowAttributes`. Its original extended style is cached before modification and restored when the session stops.

## Tracking and performance

Two different mechanisms deliberately have different responsibilities:

- Discovery runs once per second and may enumerate processes/windows.
- Movement tracking never enumerates processes.

Movement uses `EVENT_OBJECT_LOCATIONCHANGE`. Focus restoration uses `EVENT_SYSTEM_FOREGROUND`. A lightweight 16 ms position check exists only as a missed-event fallback.

The hot path reads synchronous `GetWindowRect` coordinates. DWM extended-frame bounds are measured periodically only to calculate invisible resize-border insets. This avoids a compositor-frame delay that occurs when extended-frame bounds are used directly during dragging.

The last rectangle is cached, so stationary windows do not generate `SetWindowPos` calls.

## Threading model

- The WPF UI runs on the application dispatcher thread.
- Every glass host runs on a dedicated background STA thread.
- Each host owns its form, timers, native hooks, and target state.
- `GlassHostManager` communicates shutdown through `BeginInvoke(form.Close)`.

This separation prevents a slow or unusual target application from blocking the management UI.

## Compatibility and security model

GlassForge does not inject DLLs, patch executables, scrape application content, or require administrator privileges for ordinary targets. It manipulates top-level window composition from a separate process.

The target and GlassForge normally need matching Windows integrity levels. A normal process cannot reliably alter an administrator-elevated window.

## Known limitations

- A separate backdrop window can trail by a compositor frame during very rapid movement.
- The current profile backend manages the largest window per process.
- Whole-window alpha also affects foreground content.
- Undocumented accent APIs can behave differently across Windows builds.
- Applications with custom protected composition may reject or ignore changes.

## Extension points

- Add a material enum to `AppProfile` and select different native policies in `EnableAcrylic`.
- Replace largest-window selection with a collection of per-HWND hosts.
- Add a supported in-process backend interface for applications with extension APIs.
- Extract native composition into a separate class library when additional front ends are introduced.

