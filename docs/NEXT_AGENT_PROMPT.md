# Handoff prompt: GlassForge, Office glass (Excel and PowerPoint next)

Paste everything below into a new Claude Code session. It assumes you know nothing about this project.

---

## 1. What GlassForge is, and who owns it

GlassForge is my (the user's) own Windows desktop tool. I wrote it, and I own the GitHub repository
**Jeter361/GlassForge** (public, MIT licence; `gh` CLI is logged in as Jeter361 at `C:\Program Files\GitHub CLI\gh.exe`).
It's a C# / .NET 8 WPF app that puts **live frosted glass** (a real-time blur of whatever is behind a window) behind
other Windows apps. The reason it exists: I run an **animated wallpaper (Wallpaper Engine)**, and Windows' own "Mica"
material only shows the *static* Windows wallpaper image, never the animated one. Acrylic-style live blur does show it.

I've released public betas (latest public release v0.1.3-beta; v0.1.4-beta.1 pre-release) and recruited testers on
Reddit. The README promises **"no injection, no admin"** for the default experience. Some apps can't be made glass from
outside, so I've approved an **opt-in exception**: a native hook DLL injected into specific apps, only when the user
ticks an EXPERIMENTAL toggle that carries a red warning. That code is on a **local branch that has never been
pushed** and must not be pushed without asking me (the README has to name the injection features first).

### How it works (three tiers, cheapest first)

1. **Host mode** (`GlassHostSession`): GlassForge's own acrylic window sits behind the target, and the target is made
   partly transparent. No injection.
2. **Native mode** (`NativeBackdropSession`, `TargetWindowStyler`): sets DWM attributes on the target window from
   outside: `DWMWA_SYSTEMBACKDROP_TYPE = 3` (Acrylic), optionally `DwmExtendFrameIntoClientArea` over the whole window,
   plus "keep glass when unfocused" (posts `WM_NCACTIVATE(TRUE)`). No injection. Works for apps that leave their
   background clear (Notepad, Outlook).
3. **Opt-in hook** (`native/hook`, C++ with Microsoft Detours): `GlassForge.Hook.dll`, embedded in the exe, extracted to
   `%LOCALAPPDATA%\GlassForge\hook\GlassForge.Hook.<hash>.dll`, and loaded into a target app's UI threads with
   `SetWindowsHookEx(WH_GETMESSAGE)` (`ExplorerHookSession`). Used for File Explorer, Windows Terminal (tab row only),
   Windows Settings, and now Microsoft Word.

## 2. Where we are

We're extending the live glass to more Windows apps, **one app at a time**. Work on branch **`feat/hybrid-backdrop`**
in **`C:\glass\GlassForge`**, with local commits only.

| App | State | Commit | Writeup section (docs/GLASS_WRITEUP.md) |
| --- | --- | --- | --- |
| File Explorer | Working, opt-in hook (GDI/uxtheme paint hooks + XAML diagnostics) | `cc29075` | top of file, "Update" |
| Notepad | Working, Native mode, no injection (text area set black via `EM_SETBKGNDCOLOR`) | `095b282` etc. | "Update" |
| Windows Terminal | Tab row only, opt-in frame-only hook | `b2ec689` | "Update 2" |
| Windows Settings | Working, the flagship: opt-in hook swaps its Mica brush for a live host backdrop brush | `832a571` | "Update 2: Windows Settings: working" |
| Task Manager | **Parked.** Runs elevated (High integrity), so Windows (UIPI) blocks every route from a normal-user GlassForge | `34b216a` | "Update 3" |
| Microsoft Word | **Working, confirmed by me by eye.** Opt-in hook, "Glass background (EXPERIMENTAL)" | `58a264e` | "Update 4" |
| Excel | **Working.** Confirmed by eye. Opt-in hook, same Office swap as Word | `a87b745` | "Update 5" |
| PowerPoint | **Working.** Confirmed by eye. Opt-in hook, same Office swap as Word | `a87b745` | "Update 5" |
| Microsoft Teams | **Chrome/sidebar working, chat pane parked.** Opt-in hook + host-backdrop opt-in | `ee1118c` | "Update 6" |
| Explorer "glossy" look | **Dead end.** WinUI 3 has no host-backdrop brush; the vibrant Settings look isn't reachable on Explorer | n/a | "Update 7" |
| Microsoft Outlook (new, `olk`) | **Native borders work; full glass parked** (WebView2). Classic `OUTLOOK.EXE` untested and would follow the Office path | n/a | "Update 8" |
| Later | Calculator, Paint, Snipping Tool, Clock, Photos, Media Player | | |

### How Word was solved (Excel and PowerPoint very likely work the same way)

- Office draws its **own** Mica: its UI library `mso40uiwin32client.dll` calls
  `Windows.UI.Composition.Compositor::TryCreateBlurredWallpaperBackdropBrush` (a blur of the *static* wallpaper) and
  feeds it into an effect graph (`Blend(Blend(BlurredWallpaperBackdrop, Opacity(Flood)), Opacity(Flood))`). The
  window's DWM backdrop is 0 and its surface is opaque, so Native mode shows nothing.
- `native/hook/office.cpp` detours that method (in `dcomp.dll`) and returns `ICompositor3::CreateHostBackdropBrush`
  instead (a live blur of what's behind the window). The window opts in with `DWMWA_USE_HOSTBACKDROPBRUSH`.
- **Timing is everything:** Office requests those brushes during startup, before the hooked UI thread runs any hook
  code. So `DllMain` starts the detour on its own thread (`StartOfficeHooksEarly`). Missing this race made the title
  bar look like "a different mechanism" for hours. If something "isn't affected", first check whether it was created
  before the hook loaded.
- Focus out turned the glass black; fixed by a window subclass that swallows `WM_ACTIVATE(WA_INACTIVE)` (plus
  `WM_NCACTIVATE` → TRUE and `WM_ACTIVATEAPP(FALSE)`). The DLL is pinned in the Office process.
- App side: `AppProfile.IsWord`; `HookWindowClass = AppProfile.AnyWindowClass ("*")` makes the session hook every
  Word window, splash screen included; `GlassHostManager` runs **only** the hook session for Word (no Native session).
- Limits shown in the UI: Native backdrop alone does nothing for Word; Word must be **closed and reopened** after
  turning the toggle on or off (a live switch-back exists in `office.cpp` but recorded "0 of 0 inputs").

Excel, measured: `C:\Program Files\Microsoft Office\root\Office16\EXCEL.EXE`, Medium integrity, main window class
`XLMAIN`, DWM backdrop 0, loads `mso40uiwin32client.dll`, `dcomp.dll` and `wuceffects.dll`, the same as Word.

## 3. Your job

1. Read, in this order: `C:\glass\GlassForge\CLAUDE.md`, `docs/GLASS_WRITEUP.md` (at least "Update 4: Microsoft
   Word" and "Lessons that generalize"), then `native/hook/office.cpp` and `native/hook/dllmain.cpp` (search `Office`).
2. Classify PowerPoint (`Office16\POWERPNT.EXE`): integrity level, main window class (probably `PPTFrameClass`), DWM
   backdrop, whether it loads `mso40uiwin32client.dll`. **Report to me before changing code.**
3. Generalise Word's fix to an Office list instead of copying it:
   - Hook DLL (`dllmain.cpp`): the `WINWORD.EXE` checks (DllMain early start, the message hook, the Detach path) → a list
     of `WINWORD.EXE`, `EXCEL.EXE`, `POWERPNT.EXE`. `office.cpp` `OptInOfficeWindow`: `OpusApp` → `OpusApp`, `XLMAIN`,
     and PowerPoint's class.
   - App: `IsWord` → an Office check for all three (`AppProfile.cs`, `GlassHostManager.cs`, `MainWindow.xaml` toggle
     and warning naming the right app). Keep the JSON format (`HookGlass` is still serialised as `"ExplorerGlass"`).
     Add a smoke test per app in `tests/GlassForge.Tests/Program.cs`.
4. Test each app from a **fresh launch** through GlassForge (close the app first, since Office makes its brushes at
   startup). In `hook.log`, `brush detours installed` must come before the first `wallpaper brush request`. Then ask me
   to judge by eye: the sides, title bar and tabs should be live over Wallpaper Engine and stay glass when unfocused.
5. Optional, only if I ask: make "off" restore a running Office app without a restart.

## 4. How I want you to work

- One app at a time. Classify first (Win32/GDI, WinUI 3, system XAML/UWP, Office, elevated?) and **measure before
  hooking**. Prefer no injection. I've given blanket permission to inject into any app, **except**: warn me before
  anything that could take down **Windows Terminal**. Claude Code runs inside it, and all its tabs share one process.
- I judge the look by eye; screen captures (GDI) can't see DWM glass. Ask me to look and describe, or to send a
  screenshot. My screenshots land in `C:\Users\alexw\OneDrive\Pictures\Screenshots`, and you can read exact pixel values
  from them with `System.Drawing` (that's how the static-wallpaper strip was identified). Don't take full-screen captures
  yourself: work chats are visible.
- Every hook feature stays opt-in: an EXPERIMENTAL toggle, a red warning in the profile editor, and plain wording when an
  app restart is needed. It must leave the app stable when turned off.
- Office apps keep all windows in one process: warn me about unsaved documents before closing one, and test with blank
  documents.
- I like you to push forward without asking at every step, revert cleanly if I ask, and stop and ask when you're going
  in circles.
- After each app works: commit locally (never push), add a section to `docs/GLASS_WRITEUP.md` (findings, what failed,
  final approach, what's untested), update `CLAUDE.md`'s pointers and this file's status table.

## 5. Build, run and rebuild

- **Exit GlassForge before rebuilding** (the exe is locked):
  `[System.Threading.EventWaitHandle]::OpenExisting('Local\GlassForge.ExitRequest').Set()`. Closing its window only
  hides it to the tray.
- Hook first (the app embeds the DLL): `cmake --build C:\glass\GlassForge\native\build --config Release`
  (first time only: `cmake -S native -B native\build -G "Visual Studio 17 2022" -A x64`).
- App: `dotnet build C:\glass\GlassForge\src\GlassForge.App -c Debug --source https://api.nuget.org/v3/index.json`
  (the machine has no NuGet source configured, hence `--source`).
- Tests: `dotnet run --project C:\glass\GlassForge\tests\GlassForge.Tests`, which must print `All GlassForge smoke tests passed.`
- Run: `C:\glass\GlassForge\src\GlassForge.App\bin\Debug\net8.0-windows10.0.19041.0\GlassForge.exe`. Find running
  copies by the name prefix `GlassForge*` (downloads get renamed).
- **Profiles:** `%LOCALAPPDATA%\GlassForge\profiles.json`. Edit it only while GlassForge is stopped (a running copy saves
  its older list over your edit), and back it up first (backups already there: `profiles.json.before-*.bak`).
- Gotchas: Python edits mangle backslashes in C++/C# strings (it once wrote a raw NUL byte); use the Edit tool for those
  lines. Keep each file's BOM and line endings. PowerShell 5.1 breaks `git commit -m` with double quotes, so use
  `git commit -F file`. Never commit `docs/Images/Screenshot 2026-09-24 *.png` (personal information).

## 6. Experimenting and diagnosing: the toolkit

| Tool / file | What it's for |
| --- | --- |
| `%LOCALAPPDATA%\GlassForge\hook.log` | Everything the hook DLL logs, prefixed with time and `process[pid]`. Your main diagnostic |
| `native/hook/log.cpp` (`Log(...)`) | Call it from any hook code; thread-safe |
| `native/tools/launch-hooked.ps1 -Exe <path> [-Arguments ..]` | Launches an app and hooks every UI thread from its first window. Use it for startup surveys. Stop it by creating `%LOCALAPPDATA%\GlassForge\hook.stop` |
| `native/tools/run-test.ps1 -Window <hwnd>` (uses `inject.ps1`) | Injects the freshly built DLL into one existing window's thread; stopped by the same `hook.stop` file |
| `%LOCALAPPDATA%\GlassForge\hook.survey` + `native/tools/dump.ps1 -Window <hwnd>` | Explorer-style paint-call survey counters (create the file, restart, dump, delete) |
| `native/tools/office-survey.cpp` | Reference copy of the survey build used for Word: logs brush creation, effect graphs, sprite brushes, DWM calls, file opens (kernel32 + ntdll), Direct2D colours, gradients and bitmaps, GDI blits. Copy it over `office.cpp` to reuse it |
| `native/tools/taskmgr-probe.ps1` | Example of an outside-only probe (DWM attributes + keep-active from a WinEvent hook) for an elevated app |
| Inline PowerShell `Add-Type` C# snippets | How every window was classified: `EnumWindows`/`EnumChildWindows` (class, style, parent), `DwmGetWindowAttribute` (backdrop 38, dark mode 20), the integrity level from the process token, loaded modules via `Get-Process … .Modules` |
| Set-and-restore reads | E.g. `EM_SETBKGNDCOLOR` returns the previous colour: set a sentinel and put the old value back to read a value that has no getter |
| Diagnostic colours | Temporarily feed a bright colour into a suspected brush or input and ask me what turns red (this proved which parts used Office's Mica graph) |
| `dumpbin /disasm` | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\dumpbin.exe`. Used to map `dcomp.dll`'s brush constructor (types 0 backdrop, 1 host backdrop, 2 blurred wallpaper). Run it from PowerShell; Git Bash mangles `/flags` |
| Windows SDK headers | `C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\winrt\windows.ui.composition.h` (vtable slot order for detours: IUnknown 3 + IInspectable 3, then methods in declaration order), `um\d2d1.h`, `um\d2d1_1.h` |
| Windhawk sources (read-only references, don't copy) | `C:\glass\translucent-windows.wh.cpp` (no licence), `C:\glass\windows-11-*-styler.wh.cpp` (GPL-3.0). Windhawk is installed; all its mods except a taskbar one are disabled, and `windhawk.dll` loads into every process anyway (inert) |

**Method that worked:** measure → form one hypothesis → a cheap decisive test (log it, colour it, hide it, move it) →
only then build. Log once per distinct key to keep the log readable. Every visual verdict comes from me.

## 7. Useful directories and files

| Path | What |
| --- | --- |
| `C:\glass\GlassForge` | The repo (branch `feat/hybrid-backdrop`; remote `origin` = github.com/Jeter361/GlassForge, never push) |
| `CLAUDE.md` | Agent notes and pointers (read first) |
| `docs/GLASS_WRITEUP.md` | Full engineering history: every app, what failed and why, the final approach, what's untested |
| `docs/ARCHITECTURE.md`, `docs/USER_GUIDE.md`, `README.md` | Product docs (the README promises no injection) |
| `docs/NEXT_AGENT_PROMPT.md` | This file; keep its status table current |
| `docs/GLASS_PLAYBOOK.md` | Unfinished stub, untracked; leave it |
| `native/hook/dllmain.cpp` | Hook entry point (`GlassForgeGetMessageProc`), app routing by window class and process, Explorer paint hooks, DWM detours, Attach/Detach/Dump messages |
| `native/hook/office.cpp`, `office.h` | Office (Word) glass |
| `native/hook/uwp.cpp`, `xaml.cpp` | Settings' backdrop brush swap; XAML diagnostics (Explorer, Settings) |
| `native/CMakeLists.txt`, `native/third_party/Detours` | Hook build; Detours is a git submodule |
| `src/GlassForge.App/Models/AppProfile.cs` | Profile model, the per-app flags (`IsExplorer`, `IsSettings`, `IsTerminal`, `IsWord`), `HookWindowClass` |
| `src/GlassForge.App/Services/GlassHostManager.cs` | Picks the session(s) for a profile |
| `src/GlassForge.App/Services/ExplorerHookSession.cs` | Places the hook on target threads, Attach/Detach, `HookLibrary` (extracts the embedded DLL) |
| `src/GlassForge.App/Services/NativeBackdropSession.cs`, `TargetWindowStyler.cs` | Native mode |
| `src/GlassForge.App/MainWindow.xaml` | Profile editor; each app's EXPERIMENTAL toggle and red warning block |
| `tests/GlassForge.Tests/Program.cs` | Smoke tests |
| `%LOCALAPPDATA%\GlassForge\` | `profiles.json` (+ backups), `hook.log`, `hook\` (extracted DLLs), `touched-windows.json` |
| `C:\Users\alexw\Downloads\GlassForge — Full File Explorer Glass Engineering Writeup.md` | Older copy of the writeup (the repo's `docs/GLASS_WRITEUP.md` is current) |
| `C:\docs\GlassForge_*.md` | Earlier session notes (Explorer experiment and progress, MicaForEveryone inspiration) |
| `C:\glass\*Transparency.ps1` | My old PowerShell prototypes. They can still be running and fight GlassForge, so check for them if a window misbehaves |
| `C:\Users\alexw\AppData\Local\Packages\Microsoft.WindowsTerminal_8wekyb3d8bbwe\LocalState\settings.json` | Terminal settings (theme "GlassForge glass"; backup `settings.json.before-glassforge.bak`) |

Start by reading the docs, then report PowerPoint's process details and your plan for generalising Word's fix to Excel
and PowerPoint.
