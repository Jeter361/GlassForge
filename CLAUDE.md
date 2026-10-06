# GlassForge: notes for agents

GlassForge (C# / .NET 8 WPF) puts live acrylic glass behind Windows apps. Most of it works from outside the app (DWM attributes, a glass host window). A few apps need the opt-in hook in `native/hook` (C++, Microsoft Detours), loaded only when the user ticks the app's experimental toggle.

**Read `docs/GLASS_WRITEUP.md` before changing any glass behavior.** It records what was tried for each app, what failed, and why the current approach was chosen. Work is on branch `feat/hybrid-backdrop`.

## Windows Settings glass: the flagship feature

The user considers this the best-looking feature so far. Treat it carefully and verify by eye with the user after any change; screen captures can't show DWM glass.

- **Explanation:** `docs/GLASS_WRITEUP.md`, section "Windows Settings: working". It covers why Settings showed only static Mica, what was tried, and why the final approach works and stays glass when unfocused.
- **Core code:** `native/hook/uwp.cpp` (and `uwp.h`): replacing Settings' backdrop brush, keeping it in place, and restoring it on detach or unload.
- **Hook wiring:** `native/hook/dllmain.cpp` (`t_xamlOnly`, detection of `ApplicationFrameWindow` in `SystemSettings.exe`, timer re-check, restore on detach), `native/hook/xaml.cpp` (system XAML connection via `Windows.UI.Xaml.dll`).
- **App side:** `src/GlassForge.App/Models/AppProfile.cs` (`IsSettings`, `UwpContentWindowClass`), `Services/ExplorerHookSession.cs` (finds the Settings content window inside its frame), `Services/WindowDiscoveryService.cs` (lists Settings under its own name), `MainWindow.xaml` ("Glass background" toggle and warning).
- **Commit:** `832a571` (its message says WIP; it was confirmed working afterwards).
- **Not yet verified:** turning it off restores Mica; minimize/restore; closing and reopening Settings; theme changes.

## Other apps (one at a time)

Classify each app (Win32/GDI, WinUI 3, system XAML/UWP, elevated or not) and measure before hooking. Prefer no injection, and tell the user before injecting into a new app.

- **Task Manager: parked.** It runs elevated (High integrity), so a normal-user GlassForge is blocked from every route (UIPI). An elevated probe got glass on the Processes page; Details stays grey. See `docs/GLASS_WRITEUP.md`, "Update 3: Task Manager", and `native/tools/taskmgr-probe.ps1` (run elevated).
- **Microsoft Word: working** (opt-in hook, `native/hook/office.cpp`). Office's own Mica brush is swapped for a live host backdrop brush; the detour must be in place at Word's startup (`DllMain` thread). Turning it off needs a Word restart. See "Update 4: Microsoft Word".
- **Excel and PowerPoint: working** (opt-in hook, same `office.cpp` swap as Word; `IsOfficeProcess`/`IsOffice` cover all three). See "Update 5", which also records the hook-DLL extraction race that made the first generalised launch look broken (fixed in `ExplorerHookSession.Extract`). Note: the keep-active subclass means the first click on an Office window can be spent on activation (parked, see `office.cpp` `KeepActive`).
- **Microsoft Teams: native chrome works, chat pane parked** (opt-in hook). Teams' TwinUI draws its own host-backdrop brushes for the chrome; the hook only opts the window in (`DWMWA_USE_HOSTBACKDROPBRUSH`, not settable from outside) and keeps it active. The WebView2 chat pane stays opaque; making it glass needs a script injected before Teams creates its WebView2 environment, which a window-triggered hook can't catch (parked). See "Update 6".
- **Explorer "Settings-style glossy blur": dead end** (`docs/GLASS_WRITEUP.md`, Update 7). Windows Settings is vibrant because it uses a host backdrop brush (`CreateHostBackdropBrush`); that API exists only in the system `Windows.UI.Composition`. Explorer's WinUI 3 content uses `Microsoft.UI.Composition`, which has no host-backdrop brush (only `CreateBackdropBrush`, which samples the app's own background), so the tinted/static look can't be turned into the live untinted one. Do not re-run this road.
- **Microsoft Outlook (new client `olk`): borders only** (Update 8). WebView2 like Teams; full glass is parked. Classic `OUTLOOK.EXE` is untested and would follow the Office path.
- **Any DXGI app (generic color-to-alpha glass): experimental, parked** (`native/hook/dxgiglass.cpp`, dev tools `native/tools/keep-areas.ps1`, launcher `LaunchWithGlass`). Worked on LogMeIn Resolve (Qt); remote screen kept solid via UI Automation rectangles. See "Update 9" for every pitfall before touching it.
- **Next:** Calculator, Paint, Snipping Tool, Clock, Photos, Media Player. Handoff prompt: `docs/NEXT_AGENT_PROMPT.md`.

## Working on this repo

- Build the hook before the app (the app embeds the DLL): `cmake --build native/build --config Release`, then `dotnet build src/GlassForge.App -c Debug --source https://api.nuget.org/v3/index.json`, then `dotnet run --project tests/GlassForge.Tests`.
- Exit a running GlassForge first (the exe is locked): set the named event `Local\GlassForge.ExitRequest`.
- Edit `%LOCALAPPDATA%\GlassForge\profiles.json` only while GlassForge is stopped; a running copy can save its older list over your edit.
- The repo is public and the README promises "no injection" for the default experience. Ask before pushing; the README must name the opt-in hook features first.
- Never commit `docs/Images/Screenshot 2026-09-24 *.png` (personal information).
