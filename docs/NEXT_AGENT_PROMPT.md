We're continuing the GlassForge project: extending the live-glass work we did for Windows Settings to more Windows
apps, one at a time. Work in C:\glass\GlassForge on branch feat/hybrid-backdrop (local commits only, never push; the
repo is public).

## Where things stand (Oct 1, 2026, night)

| App | State | Commit | Writeup section |
| --- | --- | --- | --- |
| Explorer, Notepad, Terminal tab row, Settings | Working (earlier work) | up to `832a571` | "Update", "Update 2" |
| Task Manager | **Parked.** Runs elevated (High integrity), so UIPI blocks every route from a normal-user GlassForge. An elevated probe gave glass on Processes; Details stays grey. Option if resumed: an opt-in elevated helper | `34b216a` | "Update 3: Task Manager" |
| Microsoft Word | **Working, user-confirmed** ("looks great", title bar included). Opt-in hook "Glass background (EXPERIMENTAL)" on `WINWORD` profiles | `58a264e` | "Update 4: Microsoft Word" |
| Excel | **Next, measured only** (see below) | none | none yet |
| PowerPoint | **Next, not measured yet.** The first launch attempt showed no process; launch it again and classify it | none | none |

How Word works (read `native/hook/office.cpp` and "Update 4" before touching it):
- Office draws its own Mica in `mso40uiwin32client.dll` from `Compositor::TryCreateBlurredWallpaperBackdropBrush` (a
  blur of the static wallpaper). The hook detours it and returns `CreateHostBackdropBrush` (a live blur of whatever is behind the window).
- Office requests those brushes during startup, before the hooked UI thread runs hook code, so `DllMain` starts the detour
  on its own thread (`StartOfficeHooksEarly`). Missing this race made the title bar look like "something else" for hours.
- The window opts in with `DWMWA_USE_HOSTBACKDROPBRUSH`; a subclass swallows `WM_ACTIVATE(WA_INACTIVE)` (plus
  `WM_NCACTIVATE`/`WM_ACTIVATEAPP`) so the glass doesn't go black on focus out. The DLL is pinned in the Office process.
- App side: `AppProfile.IsWord`, `HookWindowClass = AnyWindowClass ("*")` (hook every Word window, splash included),
  and `GlassHostManager` runs only the hook session for Word, with no Native session.
- Known limit: turning it off needs an Office restart. The live switch-back in `office.cpp` recorded 0 inputs; the UI
  says "close and reopen". Native backdrop alone does nothing for Word, and the UI says so.

Excel measurements so far: `EXCEL.EXE` (Office16, Click-to-Run), Medium integrity, main window class `XLMAIN`,
DWM backdrop 0, loads `mso40uiwin32client.dll` + `dcomp.dll` + `wuceffects.dll`, the same as Word, so the same fix very
likely applies. Not yet confirmed: that Excel requests the same wallpaper brush.

## Your job

1. Read CLAUDE.md and docs/GLASS_WRITEUP.md (especially "Update 4: Microsoft Word"), then `native/hook/office.cpp`.
2. Classify PowerPoint (`POWERPNT.EXE`): integrity, main window class (probably `PPTFrameClass`), DWM backdrop,
   whether it loads `mso40uiwin32client.dll`. Report to the user before changing code.
3. Generalise the Word code to an Office list instead of copying it:
   - Hook DLL: the `WINWORD.EXE` checks in `dllmain.cpp` (DllMain early start, message hook) → a list
     (`WINWORD.EXE`, `EXCEL.EXE`, `POWERPNT.EXE`); `OptInOfficeWindow`'s `OpusApp` check → the matching main classes
     (`OpusApp`, `XLMAIN`, PowerPoint's).
   - App: `IsWord` → an Office check covering all three (keep the JSON format; add a smoke test per app), the toggle
     and red warning shown for each, with the warning naming the right app.
4. Test each app with a fresh launch through GlassForge (close the app first; Office makes its brushes at startup).
   Check `%LOCALAPPDATA%\GlassForge\hook.log` for `brush detours installed` before the first `wallpaper brush request`,
   then ask the user to judge by eye: sides, title bar and tabs live over Wallpaper Engine; stays glass on focus out.
5. Optional, if the user wants it: make the off switch restore a running Office app (find where the swapped brushes
   are plugged in; the earlier survey saw `SetSourceParameter` only for the second brush of each pair). The survey
   build of `office.cpp` (effect-graph, D2D, GDI, file-open logging) is described in "Update 4" if needed again.

## How I want you to work

- One app at a time. Prefer the no-injection route; the user has given blanket permission to inject into any app
  (no need to ask), but warn before anything that could take down Windows Terminal (Claude Code runs in it).
- The user judges the look by eye; screen captures can't see DWM glass. Ask them to look and describe it, or send a
  screenshot (you can read pixel values from their screenshot files in OneDrive\Pictures\Screenshots).
- Every hook feature stays opt-in, with an EXPERIMENTAL toggle and a red warning in the profile editor; say plainly in
  the UI when something needs an app restart.
- Office apps hold all windows in one process: warn about unsaved documents before closing or crashing one; test with
  blank documents.
- Exit GlassForge before rebuilding (set the event `Local\GlassForge.ExitRequest`) and don't edit profiles.json while it's
  running (back it up first). Build order: `cmake --build native/build --config Release`, then
  `dotnet build src/GlassForge.App -c Debug --source https://api.nuget.org/v3/index.json`, then
  `dotnet run --project tests/GlassForge.Tests`.
- Python edits mangle backslashes in C++/C# strings (it wrote a raw NUL byte once); use the Edit tool for those lines.
- After each app works: commit locally (don't push), add a section to docs/GLASS_WRITEUP.md (findings, what failed,
  final approach, what's untested), update CLAUDE.md's pointers, and update this file's status table.

Start by reading the docs, then report PowerPoint's process details and your plan for generalising Word's fix.
