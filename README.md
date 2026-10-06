# GlassForge

GlassForge is a Windows desktop tool that applies a configurable frosted-glass backdrop to Microsoft and third-party applications without modifying their binaries. Users select a running application, create a process-restricted profile, and tune window opacity, tint strength, and tint color.

The current foundation is based on working Teams, Outlook, and Word PowerShell prototypes, but the product code is native C#.

> **Public beta — contributors and testers wanted.** GlassForge works day to day, but it needs broader coverage across apps, Windows builds, GPUs, and multi-monitor/DPI setups. See [Get involved](#get-involved).

![GlassForge applying frosted glass to Chrome](docs/Images/glassforge-demo.gif)

## Download

**[ Download the latest GlassForge.exe](../../releases/latest)** — one self-contained file for Windows 10/11 x64. No installer and no .NET install required. GlassForge is open source under the MIT license, so you can also build it yourself (see [Build and run](#build-and-run)).

The beta isn't code-signed yet, so Windows SmartScreen may warn you the first time. Click **More info → Run anyway**.

**Updating:** just run the new `GlassForge.exe`. It closes any older copy that's still running, restores that copy's windows, and takes over its launch-at-startup entry.

## Screenshots

Tune tint color and opacity live, with changes applied to the target app instantly:

![Tuning Chrome's glass tint in GlassForge](docs/Images/glassforge-tint-demo.gif)

| Microsoft Edge | Microsoft Word |
|---|---|
| ![Edge with GlassForge](docs/Images/edge.png) | ![Word with GlassForge](docs/Images/word.png) |
| **Outlook (To Do)** | **Spotify** |
| ![Outlook To Do with GlassForge](docs/Images/outlook-todo.png) | ![Spotify with GlassForge](docs/Images/spotify.png) |

## Current capabilities

- Discover visible desktop applications.
- Create profiles for any process, including apps that report no `MainWindowHandle`.
- Apply background acrylic through a dedicated native host window.
- Keep application text, icons, and controls on the sharp foreground layer.
- Track movement with WinEvent hooks and DPI-correct native coordinates.
- Restore backdrop Z-order immediately when a target regains focus.
- Persist profiles as JSON under `%LOCALAPPDATA%\GlassForge`.
- Run multiple independent profiles.
- Minimize to the notification area.
- Start silently with Windows through the current-user Run key.
- Go solid automatically while a target is fullscreen (video, games, slideshows).
- Optionally go solid while the target app plays media, per profile ("Pause while media plays").
- Allow only one running instance, and restore any windows left translucent by a crash on the next launch.
- Two modes per profile: **Glass host** (works with any app) or **Native backdrop**, where Windows draws live Acrylic on the app's own frame with zero overhead and keeps it live when the app loses focus (Windows 11 22H2+). Apps that leave their background clear, like Notepad and Windows Terminal, can be glass edge to edge; apps that paint their own background (File Explorer's file list, Outlook, Chrome) get it on the frame only.
- Per-profile window options in either mode: corner style (default, round, small, square) and a forced dark or light title bar.
- Pick up a newly launched app's first window immediately instead of on the next polling pass.
- Notepad: full-window glass behind the text, plus a **Solid text area (for HDR)** option for HDR displays, where Windows would otherwise make the typed text invisible.
- **Experimental, opt-in per app (injects code, see [Safety model](#safety-model)):** live glass for File Explorer (whole window), Windows Terminal (tab row), Windows Settings, Word, Excel, PowerPoint and Teams (native chrome; the chat pane stays solid). Each one is off by default behind its own toggle and red warning.
- **Experimental "Glass inside the app" (injects code):** for apps that paint every pixel with Direct3D (Qt, Chromium/Electron), where Native glass shows nothing, the app's own background color is turned into glass on every frame while text, pictures and video stay solid. Tested on LogMeIn Resolve and ChatGPT. Turning it on or off restarts the app.
- **Diagnose this app:** a read-only check of how an app is built and how it draws (framework, GDI or Direct3D, Store package, administrator) that recommends the glass mode that fits and applies it with one click.
- A minimal editor: one list of apps with on/off switches, changes apply live.

## Requirements

- Windows 11 is recommended.
- Nothing extra for normal use; the release `.exe` bundles the .NET runtime.
- .NET 8 SDK for development.
- The target app and GlassForge should run at the same integrity level.

## Build and run

```powershell
dotnet build GlassForge.sln
dotnet run --project src\GlassForge.App
```

Run the UI without starting configured effects:

```powershell
dotnet run --project src\GlassForge.App -- --no-effects
```

Run smoke tests:

```powershell
dotnet run --project tests\GlassForge.Tests
```

## Repository layout

```text
src/GlassForge.App/
  Models/       Persistent application profiles
  Native/       Win32 and DWM interop declarations
  Services/     Discovery, persistence, startup, and glass-host engine
  App.xaml      Application lifecycle and background mode
  MainWindow.*  Profile management UI and tray integration
native/hook/
  Opt-in hook DLL (C++, CMake) for the experimental per-app glass
tests/GlassForge.Tests/
  Dependency-free foundation smoke tests
```

## Documentation

- [User guide](docs/USER_GUIDE.md)
- [Software architecture](docs/ARCHITECTURE.md)
- [PowerShell prototype migration](docs/SCRIPT_MIGRATION.md)

## Architecture

Each enabled profile owns an isolated STA host thread. The host finds the largest visible top-level window owned by the configured process, applies layered opacity to the target, and positions a non-activating acrylic surface directly behind it. Target discovery is throttled; movement and focus changes use WinEvent callbacks. Frame coordinates are cached so stationary windows produce no compositor repositioning work.

This design is broadly compatible with Win32, WebView2, Chromium, and Office applications. Because the acrylic surface is a separate HWND, a small compositor-frame delay can remain during rapid dragging. A future injected or in-process composition backend could remove that limitation for applications that expose a supported extension point.

## Roadmap

- Multiple-window support per process.
- Presets and import/export.
- Per-monitor and per-state tuning.
- An automatic mode that tries the native backdrop first and falls back to the glass host when the app paints over it.
- Signed installer and automatic updates.
- Accessibility and high-contrast validation.
- Crash recovery, structured logging, and diagnostics export.
- Optional supported in-process backends for applications with extension APIs.
- Automated UI and native-window integration tests.

## Acknowledgements

Native backdrop mode, the window options, and the window-eligibility rules are inspired by
[MicaForEveryone](https://github.com/MicaForEveryone/MicaForEveryone) (MIT).

## Get involved

**Testers:** build and run GlassForge, point it at the apps you use, and [open an issue](../../issues/new/choose) using the bug report template. The most useful reports include the target app and version, your Windows build, your monitor/DPI setup, and a screenshot or short clip.

**Developers:** read [CONTRIBUTING.md](CONTRIBUTING.md) and [the architecture doc](docs/ARCHITECTURE.md), then pick anything from the roadmap or the issue tracker. Please open an issue before starting large changes.

## Safety model

GlassForge targets windows by owning process ID. By default it does not inject code, patch application files, read application content, or require administrator privileges for ordinary applications. Glass host and Native backdrop modes only use public Win32 and DWM APIs from outside the app.

**The exception is the experimental per-app hook.** For File Explorer, Windows Terminal, Settings, Word, Excel, PowerPoint and Teams, a profile can opt in to loading GlassForge's hook DLL (`native/hook`) into that app to redraw or re-enable its backdrop. It is off unless you turn it on, each toggle carries a warning, and turning it off restores open windows. Know the risks before enabling it: a bug can crash or freeze the target app (for Explorer, also the taskbar and desktop; for Office, save your documents first), antivirus may flag it, it can break after Windows or app updates, and it can conflict with other mods such as Windhawk or StartAllBack. Some apps need a restart to fully drop the hook.

**"Glass inside the app" goes further and can be turned on for any app.** It loads the same hook DLL into the app from its first instruction, so GlassForge restarts the app to apply it (open work in it is closed): a normal app is started through the hook, and a Store app through Windows' package debug settings (`IPackageDebugSettings`), which start each launch paused while the hook attaches - turning the option off clears that setting and restarts the app normally. Chromium-based apps also get the hook in their GPU process. GlassForge finds areas to keep solid (pictures, embedded video) with UI Automation, which can make some apps do extra accessibility work. All the risks above apply, to whichever app you enable it for.

## License

MIT. See `LICENSE`.
