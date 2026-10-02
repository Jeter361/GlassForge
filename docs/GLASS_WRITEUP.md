# GlassForge — Full File Explorer Glass: Engineering Writeup

Oct 1, 2026 · @rude

## Summary

File Explorer is now fully translucent in GlassForge, visually matching Windhawk's translucent-windows + Windows 11 File Explorer Styler combo, as an opt-in, clearly warned profile option. The user confirmed the final look by eye on 2026-10-01: glass behind the file list, navigation pane, Home, Gallery, toolbar and status bar; translucent scrollbars; clean (non-glowing) text; translucent hover/selection highlights.

- **Option:** "Full File Explorer glass" (EXPERIMENTAL) on `explorer` profiles in Native mode, with a red warning that it injects code into explorer.exe.
- **Mechanism:** `GlassForge.Hook.dll` (native C++, Microsoft Detours) loaded per Explorer window thread via `SetWindowsHookEx(WH_GETMESSAGE)`; it redraws GDI/uxtheme paint calls with real alpha and clears WinUI backgrounds through XAML diagnostics.
- **Also fixed:** Notepad's Save As dialog breaking under extend-into-client; removed the unused "See-through background" option.
- **State:** all uncommitted on `feat/hybrid-backdrop` in `C:\glass\GlassForge`. Live-tested only on `/separate` Explorer windows, never the taskbar/shell process.

No Windhawk code was copied; its sources (translucent-windows 1.8.2, no license; Explorer Styler, GPLv3) were read to understand the techniques, then reimplemented.

## What we learned: root causes

The single most important fact: **GDI writes 0 into every pixel's alpha byte, and inside a DWM frame extended over the client area (`DwmExtendFrameIntoClientArea` with margins `{-1,-1,-1,-1}`) DWM reads pixels as premultiplied.** So black (0,0,0,0) is fully see-through, which is why swapping background fills to black works, but any colored pixel with alpha 0 is *added* onto the backdrop. Every visual defect except the scrollbars traced back to this.

| Symptom | Cause | Fix (ours) | Windhawk equivalent |
| --- | --- | --- | --- |
| Dark scrollbars | We only blackened `ScrollBar` track parts 4–7. Arrows (part 1), thumbs (2/3) and grippers (8/9) are opaque dark theme bitmaps. | `PaintScrollBar`: glass gutter + D2D translucent pill thumb + D2D arrow triangles; grippers dropped | `PaintScroll`, `PaintScrollBarArrows`, cached D2D DIBs + nine-grid `AlphaBlend` |
| Glowing / bold text | GDI text has alpha 0 → additive over the blur; ClearType fringes glow | `DrawAlphaText`: render white-on-black offscreen, luma → coverage alpha (gamma 1/1.4), premultiply text color, `EndBufferedPaint` alpha-blend | `HookedExtTextOutW` + `ExtTextOutComposition` (gamma LUT) |
| Washed-out tooltips / colored fills | Same alpha-0 additive effect on solid fills | `FillOpaque`: fill in a buffered paint, `BufferedPaintMakeOpaque` | `HookedSHThemeFillTextRect`, `HookedFillRectClr` (highlight only) |
| Grey selection / hover tiles | Dark theme `ListView` part 1, part 6 and `TreeView` part 1 artwork is an opaque grey tile | `PaintHighlight`: translucent rounded rect (+ accent pill on tree selection) | `PaintListView`, `PaintTreeViewButton` |
| Home / details pane / nav bar solid | WinUI `Background` brushes on named grids | XAML diagnostics sets `Background=Transparent` on `HomeViewRootGrid`, `DetailsViewControlRootGrid`, `NavigationBarControlGrid` | Explorer Styler "Translucent Explorer11" theme targets the same names |
| Notepad Save As broken | App's Native mode extended the frame into the `#32770` dialog | Skip extend-into-client for `#32770` | n/a |

Other lessons:

- Explorer Styler's backdrop is identical to ours: `DWMWA_SYSTEMBACKDROP_TYPE = 3` (`DWMSBT_TRANSIENTWINDOW`, Acrylic) plus whole-window margins, re-forced by hooking `DwmSetWindowAttribute` / `DwmExtendFrameIntoClientArea` because Explorer re-applies Mica Alt and top-only margins on activation and resize.
- Theme part IDs only mean something together with the theme class. The class comes from the undocumented `uxtheme.dll` ordinal 74 (`GetThemeClass`).
- **Explorer runs each folder window on its own UI thread.** Anything process-global (detours, XAML diagnostics) must be shared; anything per-window must be per-thread.
- XAML diagnostics (`InitializeXamlDiagnosticsEx` from `Microsoft.Internal.FrameworkUdk.dll`) retains the TAP DLL until the process exits; `DllCanUnloadNow` returns `S_FALSE`. A detached DLL therefore stays loaded but inert.
- XAML property calls must run on the element's own thread; `OnVisualTreeChange` arrives on each element's thread, and `SetProperty` can re-enter it synchronously (hence a recursive mutex).
- GDI screen captures can't see DWM glass, so every visual check was done by eye.
- The survey counters (call + module + theme class + part/state + size) were how we found the scrollbar and selection parts. Measure first, then hook.

## Hook DLL: code reference

The DLL lives in `native/hook/` and builds with `native/CMakeLists.txt` (x64, static CRT, Detours as a static lib, links `user32 gdi32 dwmapi uxtheme comctl32 d2d1 ole32 oleaut32 uuid`). Output: `native/build/Release/GlassForge.Hook.dll`.

### dllmain.cpp: state

| Symbol | Role |
| --- | --- |
| `g_installed`, `g_installLock` | Detours are process-wide, installed once by the first hooked thread, removed when the last styled window detaches |
| `g_roots` (`HWND → original backdrop`), `g_rootLock` | Every styled Explorer window in the process; read by the DWM hooks |
| `thread_local t_root`, `t_seen`, `t_detached` | Per-window-thread state (implicit TLS works in dynamically loaded DLLs on Vista+) |
| `g_ownedDCs` | Memory DCs created (via `CreateCompatibleDC`) from a target DC, so offscreen buffers count as ours |
| `BackgroundColors[]` | Explorer dark fills from the survey: `#191919` main, `#1C1C1C` details pane, `#131313` nav pane |
| `g_coverageAlpha[256]` | Text gamma table: `pow(i/255, 1/1.4)` |
| `g_d2d` | `ID2D1Factory` (multi-threaded), created in `Install` |
| `g_survey` | Survey counters on only if `%LOCALAPPDATA%\GlassForge\hook.survey` exists at install; the `Count(key)` macro skips building the string otherwise |

### dllmain.cpp: functions

| Function | What it does |
| --- | --- |
| `IsTargetDC(dc)` | True if the DC's window root is this thread's `t_root`, or it is an owned memory DC |
| `HasAlphaChannel(dc)` | Window DC, or a memory DC with a 32 bpp bitmap selected; alpha work is skipped otherwise |
| `Hook_FillRect` | Background color → black (glass); other solid non-black → `FillOpaque` |
| `Hook_PatBlt` | `PATCOPY` with a background-colored brush → black |
| `Hook_ExtTextOutW` | Opaque cell: background → black, other color → `FillOpaque`; then glyphs via `DrawAlphaText`; falls back to GDI for `TA_UPDATECP`, empty clip rects or non-32bpp DCs |
| `TextBounds` | Text extent incl. `lpDx`/`ETO_PDY`, `TA_BASELINE/BOTTOM/CENTER/RIGHT` alignment, overhang padding, `ETO_CLIPPED` intersection |
| `DrawAlphaText` | `BeginBufferedPaint(BPBF_TOPDOWNDIB, BPPF_ERASE\|BPPF_NOCLIP, AC_SRC_ALPHA blend)`; copy font, align, layout, char extra; draw white; per pixel `alpha = lut[(r+2g+b)/4]`, RGB = text color × alpha; `EndBufferedPaint(TRUE)` |
| `FillOpaque` | Buffered-paint fill via `DC_BRUSH`, `BufferedPaintMakeOpaque`, blit back |
| `PaintScrollBar` | Tracks/size box → black; grippers → nothing; thumbs → pill (30% of bar thickness, 45% on hover/hot/pressed; grey 0.5–0.65 alpha); arrows → triangle by `ABS_*` direction/look; D2D `ID2D1DCRenderTarget` bound to the DC |
| `PaintHighlight` | Buffered paint + D2D rounded rect (grey, radius 4 × DPI scale); optional accent pill from `DwmGetColorizationColor` |
| `PaintGlassTheme` | Routing by `ThemeClass(theme)` + part/state: `ScrollBar` → `PaintScrollBar`; `ListView` part 1 / `TreeView` part 1 (states 2, 3, 5, 6) and `ListView` part 6 (all states except `LVGH_OPEN`/`LVGH_CLOSE`) → `PaintHighlight`; normal `Header` items and `PreviewPane` separators → black. Applies the clip rect with `SaveDC`/`IntersectClipRect` |
| `Hook_DrawThemeBackground(Ex)` | Calls `PaintGlassTheme`, else the original (`Ex` only for no options or `DTBG_CLIPRECT`) |
| `Hook_DwmSetWindowAttribute` / `Hook_DwmExtendFrameIntoClientArea` | For windows in `g_roots`: remember Explorer's requested backdrop, force Acrylic (3) and whole-window margins |
| `RootSubclass` | `WM_NCACTIVATE` always TRUE (glass stays live when unfocused); `WM_TIMER` (1 s) → `RefreshXamlStyling` |
| `ApplyGlass(root)` | Record in `g_roots`, set `t_root`, Acrylic + margins, subclass, redraw, `AddXamlThread`, start timer |
| `RemoveGlass()` | `RemoveXamlThread`, restore backdrop, zero margins, `WM_DWMCOMPOSITIONCHANGED`, redraw; returns whether other windows remain |
| `Install()` / `Uninstall()` | Idempotent Detours transaction (BitBlt/AlphaBlend survey hooks only when surveying) |

Row-state numbers (`LISS_*` and `TREIS_*` share them): 1 normal, 2 hot, 3 selected, 4 disabled, 5 selected-not-focused, 6 hot-selected. Highlight alphas: hover 0.18, selected 0.28, hot-selected 0.34, unfocused 0.16, group header 0.14.

### Message protocol (`GlassForgeGetMessageProc`, exported)

Installed per thread with `SetWindowsHookEx(WH_GETMESSAGE)`. On any message for a `CabinetWClass` root, when the thread isn't styled and isn't detached: `Install()` then `ApplyGlass`. Registered messages:

| Message | Effect |
| --- | --- |
| `GlassForge.Hook.Attach` | Clears `t_detached` (needed because the DLL stays loaded across GlassForge restarts) |
| `GlassForge.Hook.Detach` | `RemoveGlass` for this thread; `Uninstall` if no styled window is left |
| `GlassForge.Hook.Dump` | Writes survey counters to `hook.log` |

`DllMain(DLL_PROCESS_DETACH)` calls `Uninstall` as a safety net so detours never point into an unloaded DLL.

### xaml.cpp (WinUI backgrounds)

- `AddXamlThread(module, thread)`: adds the thread to `targetThreads`; the first call starts `Connect` on a worker, which loops `WinUIVisualDiagConnection1..64` through `InitializeXamlDiagnosticsEx` with TAP CLSID `{3546f878-8a67-4301-b921-2151318c4529}`.
- `Tap` (`IObjectWithSite`, `IVisualTreeServiceCallback`): `SetSite` gets `IVisualTreeService2`/`IXamlDiagnostics` and advises on a worker thread. `OnVisualTreeChange` records every node with its thread (so windows hooked later still know their elements) and styles only nodes on target threads.
- `CommandContainer` picks the elements: Gallery host chain (`GalleryViewControl`, `GalleryRootGrid` and ancestors up to 8 deep), `HomeViewRootGrid`, `DetailsViewControlRootGrid`, `NavigationBarControlGrid`, `CommandBarControl`, `CommandBarControlRootGrid`, `FileExplorerCommandBar` / `FileExplorerSecondaryCommandBar` and their `LayoutRoot`/`ContentRoot`.
- `Style` finds `Background` in `GetPropertyValuesChain`, saves the original (local vs. style value), creates one `Transparent` brush instance, and calls `SetProperty`. `Refresh(thread)` re-applies every second because Explorer rebuilds controls.
- `RemoveXamlThread` restores only the calling thread's records (`SetProperty` original or `ClearProperty`); `StopXamlStyling` unadvises all taps once no window is left.

## App integration (C#, `src/GlassForge.App`)

The option runs as a second session next to the existing Native backdrop session, so turning it off falls back to plain Native mode.

&#91;embedded content: hook architecture · app, per-window threads, shared hooks\]

GlassForge installs one message hook per Explorer window thread; inside Explorer, each thread keeps its own window state while the Detours paint hooks and the XAML diagnostics connection are shared by the whole process.

| File | Change |
| --- | --- |
| `Models/AppProfile.cs` | `ExplorerGlass` (bool, saved to JSON); `IsExplorer` (`[JsonIgnore]`, `ProcessName == "explorer"`, change-notified with `ProcessName`) |
| `Services/GlassHostManager.cs` | `GlassMode.Native when profile.IsExplorer && profile.ExplorerGlass && HookLibrary.IsAvailable` → `CompositeSession(NativeBackdropSession, ExplorerHookSession)`. Composite disposes in reverse, so the hook restores Explorer's frame before the native session restores its attributes |
| `Services/ExplorerHookSession.cs` (new) | `ExplorerHookSession` (STA thread + WinForms message loop, same shape as `NativeBackdropSession`), `ExplorerHookWorker`, `HookLibrary` |
| `Native/NativeMethods.cs` | Added `RegisterWindowMessage`, `SetWindowsHookEx`, `UnhookWindowsHookEx`, `LoadLibrary`, `GetProcAddress`, `GetClassName` |
| `MainWindow.xaml` | Toggle + EXPERIMENTAL badge + red warning box inside the Native-only Material section, visible only when `IsExplorer`; `ExplorerHookMissingText` when the build lacks the hook |
| `MainWindow.xaml.cs` | Shows `ExplorerHookMissingText` when `!HookLibrary.IsAvailable` |
| `GlassForge.App.csproj` | `EmbeddedResource` of `..\..\native\build\Release\GlassForge.Hook.dll` (`LogicalName=GlassForge.Hook.dll`), conditional on the file existing |

### ExplorerHookWorker

1. `HookLibrary.Extract()` writes the embedded DLL to `%LOCALAPPDATA%\GlassForge\hook\GlassForge.Hook.<first 12 hex of SHA-256>.dll`. A content-hashed name means a copy Explorer still holds is never overwritten.
2. `LoadLibrary` + `GetProcAddress("GlassForgeGetMessageProc")` in GlassForge itself, because `SetWindowsHookEx` needs the procedure address in the calling process.
3. For every top-level `CabinetWClass` window (startup `EnumWindows`, `EVENT_OBJECT_SHOW` WinEvent, 3 s sweep): one `SetWindowsHookEx(WH_GETMESSAGE, proc, module, threadId)` per thread, then post `GlassForge.Hook.Attach` + `WM_NULL` to wake it.
4. The sweep unhooks entries whose window is gone, so a recycled thread ID gets a fresh hook.
5. `Shutdown`: post `GlassForge.Hook.Detach` to every window, sleep 800 ms so each thread restores itself, then `UnhookWindowsHookEx` and `WM_NULL`.

`HookLibrary.IsAvailable` requires `Environment.Is64BitProcess` and the embedded resource.

### Warning text shown to users

> Warning: this injects code into File Explorer. Unlike everything else in GlassForge, this loads a GlassForge DLL into explorer.exe to redraw its windows. Explorer also runs your taskbar and desktop, so a bug here can crash or freeze them (Windows restarts Explorer automatically). Antivirus may flag it, it can break after Windows updates, and it can conflict with Windhawk, StartAllBack or other Explorer mods. Turning it off restores open windows; a leftover copy of the DLL stays loaded but inactive until Explorer restarts or you sign out.

This matters for the README promise of "no injection, no admin": the feature is the one deliberate, opt-in exception, and the README should say so before release.

## Other changes this session

**Notepad Save As dialog fix.** The Notepad profile (Native + Acrylic + ExtendIntoClient) extended the frame into every top-level Notepad window, including the common file dialog. Its GDI contents have alpha 0, so they went see-through and glowed. `TargetWindowStyler.Apply` now skips `DwmExtendFrameIntoClientArea` when `IsDialog(window)` (`GetClassName == "#32770"`); dialogs keep the Acrylic title bar only.

**See-through background removed** at the user's request ("really no use"). Removed:

- `AppProfile.KeyBackground`, `KeyColor`, `HasValidKeyColor`, `KeyColorHex`
- The BACKGROUND section in `MainWindow.xaml`, the `DetectKeyColor` handler, and the Save & Apply key-color validation
- `Services/BackgroundColorDetector.cs` (deleted)
- The keyed `CompositeSession` branch in `GlassHostManager`; `GlassHostSession.cs` reverted to HEAD (companion mode, click forwarding, `DesiredLayering`)
- `NativeMethods`: `LwaColorKey`, `WmMouseActivate`, `MaNoActivate`, `Wm{L,R,M}ButtonDown`, `PwRenderFullContent`, `SetForegroundWindow`, `PrintWindow`
- The detector and key-color smoke tests

Old `profiles.json` entries with `KeyBackground`/`KeyColor` still load; System.Text.Json ignores unknown properties.

## Build, test and results

Build order matters: the native DLL must exist before the app builds, or the option ships unavailable.

```powershell
cmake -S C:\glass\GlassForge\native -B C:\glass\GlassForge\native\build -G "Visual Studio 17 2022" -A x64
cmake --build C:\glass\GlassForge\native\build --config Release
dotnet build C:\glass\GlassForge\src\GlassForge.App -c Debug --source https://api.nuget.org/v3/index.json
dotnet run --project C:\glass\GlassForge\tests\GlassForge.Tests
```

Stop a running GlassForge before rebuilding (the exe is locked): set the named event `Local\GlassForge.ExitRequest`.

**Safe test loop** (used throughout; never the shell process):

1. Close all normal File Explorer windows; open `explorer.exe /separate,C:\Windows` and confirm its PID differs from the shell (`GetShellWindow`).
2. Set `"ExplorerGlass": true` on the `explorer` profile (backup: `profiles.before-explorerglass.json`) and launch GlassForge.
3. Check `%LOCALAPPDATA%\GlassForge\hook.log` for `detours installed (0)`, `glass applied to … on thread …`, `XAML diagnostics connection 00000000`.
4. Survey: create `hook.survey`, restart GlassForge, use the window, then `native\tools\dump.ps1 -Window <hwnd>`. Delete the file afterwards.
5. Dev loader without the app: `native\tools\run-test.ps1 -Window <hwnd>`, stopped by creating `hook.stop`.

| Check | Result |
| --- | --- |
| Survey after scrollbar fix | All `ScrollBar` parts 1, 2, 3, 5, 7, 8, 9 routed to `GlassScrollBar`; zero `ExtTextOut-fallback` |
| Survey after highlight fix | `GlassHighlight` hits for `ListView` 1 (states 2, 5, 6), `ListView` 6, `TreeView` 1 (states 2, 3, 5, 6) |
| Enable via app | DLL extracted as `GlassForge.Hook.A95D15B3D430.dll`, only the test window's thread hooked |
| Exit GlassForge | XAML restores (two `80070490` = elements Explorer already rebuilt), `glass removed`, `detours removed` |
| Relaunch | Re-attached to the same window with the DLL still loaded (Attach message path) |
| Stability | No Explorer crash in any run; shell PID 12868 never touched |
| Smoke tests | `All GlassForge smoke tests passed.` |
| Visual (user, by eye) | Confirmed: scrollbars, text, highlights, Home, Gallery all look right |

## Limitations and next steps

The biggest open item is that the feature has never run inside the shell Explorer process, which is where normal folder windows live.

**Known limitations**

- Dark mode only: `BackgroundColors[]` are the dark-theme fills; light mode would need its own survey.
- The detached DLL stays loaded until Explorer restarts (XAML diagnostics). Each GlassForge version adds another hashed copy.
- If GlassForge crashes, Windows removes the hooks and `DllMain` detach removes detours under loader lock. Windows keep Acrylic + extended frame until they are next activated or resized.
- `DrawTextWithGlow` (uxtheme ordinal 126) isn't hooked; menus or the classic address bar may double-compose text. Windhawk bypasses it.
- Windhawk's translucent-windows running at the same time would fight these hooks.
- `g_ownedDCs` only shrinks on `DeleteDC`, so cached buffered-paint DCs stay marked.

**Next steps**

- [ ] Test two windows in one process (Ctrl+N in the test window), then the shell process with the user's go-ahead
- [ ] Toggle the option on and off through the UI checkbox + Save & Apply, not only app exit
- [ ] Verify resize, navigation, tab switching, focus changes, and Explorer restart while hooked
- [ ] Publish build with the native DLL built first; check the single-file exe still embeds it
- [ ] Update the README: name this as the one opt-in injection feature
- [ ] Decide whether to hook `DrawTextWithGlow` and recheck context menus
- [ ] Commit to `feat/hybrid-backdrop` (nothing is committed yet; Detours is a submodule)

---

# Update: later on Oct 1, 2026

Everything above still holds; this section adds what happened after it was written and supersedes its **State** line and the open items. In short: Explorer glass now runs in the user's real shell Explorer with several windows, two multi-window bugs were fixed, Notepad gets the same clean glass **without injection**, and all work is committed locally (not pushed).

## Commits on `feat/hybrid-backdrop` (local only, not pushed)

| Commit | What |
| --- | --- |
| `81ee410` | Re-extend the glass when an app resets its frame |
| `ae2bd04` | Keep Notepad's text area clear after it re-themes |
| `095b282` | Clear the milky haze behind Notepad's text in Native mode |
| `2cd2573` | Turn on whole-window glass for Notepad in Native mode |
| `cc29075` | Add experimental full File Explorer glass via an opt-in hook (includes the two shell-process fixes below, the Save As fix and the see-through removal) |

The user chose local commits only: the public repo (Jeter361/GlassForge) promises "no injection", so pushing the hook is a separate decision. `native/build/` was added to `.gitignore`; the two personal screenshots in `docs/Images` stay untracked on purpose.

## Explorer: two bugs found in the shell process (fixed in `cc29075`)

The user enabled the option with normal Explorer windows open, so it ran in the shell process (PID 12868) with several windows, a case the single-window `/separate` tests never covered.

| Bug | Symptom | Cause | Fix |
| --- | --- | --- | --- |
| XAML brush shared across threads | Only the first window got a transparent toolbar, address bar and Home; every later `SetProperty` failed with `80070490` (element not found) | XAML objects belong to the thread that created them, and every Explorer window has its own XAML thread. One `Transparent` brush `InstanceHandle` was created on window A's thread and reused on window B's | `xaml.cpp`: `std::map<DWORD, InstanceHandle> transparentByThread`, the brush created on the element's own thread in `Style`, erased in `Restore(thread)` because thread IDs are recycled |
| Closed windows never detached | After closing windows and stopping GlassForge, the detours stayed installed | A closed window's thread exits without ever seeing `GlassForge.Hook.Detach`, leaving a stale entry in `g_roots`, so "last window gone" never happened | `RootSubclass` handles `WM_NCDESTROY`: `RemoveGlass(true)` (forget only, no repaint on a dying window), then `Uninstall()` if it was the last one |

Verified in PID 12868: two windows on threads 28380 and 46936 each got their own brush with every change `00000000`; closing one logged `glass removed` while the other kept working (Gallery and Home included).

## Notepad: matching Explorer's glass without injection

### What Notepad is made of

Windows 11 Notepad (`Notepad` window class) is not one surface. Child windows of its main window:

| Part | Window class | How it draws |
| --- | --- | --- |
| Tab bar, menu bar, status bar, scrollbars, Settings page, title-bar back button | `Microsoft.UI.Content.DesktopChildSiteBridge` (WinUI 3 XAML islands, `WS_EX_NOREDIRECTIONBITMAP`) | Composited above the window; their XAML backgrounds are already clear (`#00000000`) |
| Text area (one per tab) | `NotepadTextBox` → `RichEditD2DPT` | Win32 rich edit drawn with Direct2D into the main window's surface |
| Main window | `Notepad` (style `0x14CF0000`, no `WS_CLIPCHILDREN`) | Its own GDI paint is only black fills |

Notepad asks DWM for Mica Alt itself (`DWMWA_SYSTEMBACKDROP_TYPE = 4`) with dark mode on.

### Why the backdrop only showed with "Extend into the whole window"

Notepad draws its own title bar (tabs and caption buttons) inside its client area, covering the DWM frame. A plain Acrylic backdrop sits behind the frame only, so nothing showed. Extending the frame (`{-1,-1,-1,-1}`) puts the backdrop behind the whole window, which Notepad leaves clear.

**Change (`2cd2573`):** `AppProfile.DrawsOwnTitleBar` (static set `OwnTitleBarApps = { "notepad" }`). Clicking **Native backdrop** on such a profile turns on `ExtendIntoClient` (`MainWindow.NativeModeChosen`, wired to the radio button's `Click`, not `Checked`: `Checked` also fires when a Native profile is merely shown and would override the user's choice). `ResetAppearance` sets `ExtendIntoClient = DrawsOwnTitleBar`. Smoke test added.

### Why the text area looked milky next to Explorer

The tab and menu area matched Explorer exactly; only the text area was hazy and bright. `EM_SETBKGNDCOLOR` (0x0443) returns the color it replaces, so a set-and-restore read showed the rich edit's background is **`#272727`**. It reaches the surface with alpha 0, so inside the extended frame DWM *adds* `#272727` onto the blur: the same alpha-0 effect as Explorer's glowing text. Black is the see-through color, exactly as for Explorer's background fills.

**Fix (`095b282`), no injection:** GlassForge sends `EM_SETBKGNDCOLOR(0, RGB(0,0,0))` to every `RichEditD2DPT` child of a styled Notepad window and restores the original color on stop. The user preferred the result ("the dark makes it easier to read"). It only runs when the frame is extended (`ClearsEditors => IsNotepad && ExtendIntoClient && SupportsSystemBackdrop`), since anywhere else black would just be black. `SendMessageTimeout(SMTO_ABORTIFHUNG, 1000)` keeps a hung Notepad from freezing the worker thread.

### Approaches tried and dropped

| Attempt | Result |
| --- | --- |
| Accent-policy acrylic with a dark tint (`SetWindowCompositionAttribute`, `WCA_ACCENT_POLICY`, state 4, `0xB0181818`) applied to Notepad's window from outside | Killed the transparency entirely ("zero transparency"); reverted with state 0 |
| Injecting the hook into Notepad and painting a premultiplied tint (`PaintTint`, `0x66101010`) on the main window under all children | Darkened everything, but the text area still looked different from the top bar. The user wanted it to *match*, not to be darker |
| XAML survey of all islands (`LogBackground` / `BrushColor` reading each brush's `Color` and `Opacity`) | Showed the islands were already clear; the haze wasn't XAML. This is what pointed to the Win32 text control |

All Notepad hook experiments were reverted (`git checkout -- native/hook`); the committed DLL is Explorer-only.

### Two follow-up bugs

| Bug | Cause | Fix |
| --- | --- | --- |
| After closing and reopening Notepad, the haze came back (`ae2bd04`) | Notepad applies its theme color to a new text area a moment *after* it appears, overwriting the black set at show time. The first version set each control only once. A control caught before Notepad themed it reported the system window color (white), which was then "restored" on exit as a glowing white text area | `ClearEditorBackgrounds` re-applies black on every pass. The latest non-black color handed back is recorded as the one to restore (`_editors[child] = previous`). `previous == GetSysColor(COLOR_WINDOW)` while `AppsUseLightTheme == 0` counts as untouched and records `#272727` instead. A **settle burst** (`_settleTimer`, 500 ms × 10) runs after a Notepad window or a `RichEditD2DPT` child appears |
| Text area stuck solid black (`81ee410`) | Notepad calls `DwmExtendFrameIntoClientArea` itself on opening and activation, shrinking the glass back to the title bar. The black text area was then simply black (before the haze fix it fell back to plain `#272727`, which is why "it went away" earlier) | `TargetWindowStyler.Reassert(window)` sets the margins again (they can't be read back) and rewrites the backdrop only if it changed. `NativeBackdropWorker` calls it on `EVENT_SYSTEM_FOREGROUND` and `EVENT_OBJECT_LOCATIONCHANGE` for styled windows, in the settle burst (now for any profile with Extend, started on focus too), and on every 3 s discovery pass |

Verified with a set-and-restore probe script: both tabs black while running, `#272727` after exit, black again after relaunch, over two full exit/relaunch cycles; a closed-and-reopened window stayed clear at 2 s and 7 s. The user confirmed the stuck-black fix by eye ("okay for now").

### Code reference (app)

| File | Symbol | Role |
| --- | --- | --- |
| `Models/AppProfile.cs` | `IsNotepad`, `DrawsOwnTitleBar`, `OwnTitleBarApps` | App detection by process name |
| `Services/NativeBackdropSession.cs` | `_editors` (`HWND → original color`), `NotepadEditorClass = "RichEditD2DPT"`, `NotepadEditorBackground = 0x272727` | Text controls touched and what to restore |
| | `ClearEditorBackgrounds(window)`, `SetEditorBackground`, `AppsUseDarkTheme` | Re-apply black, record originals |
| | `Settle()`, `_settleTimer`, `_settleTicks` | 5 s burst of re-applies after show or focus |
| | `OnWindowEvent` | Foreground → `Reassert` + `Settle`; location change → `Reassert`; child show of a text control → `Settle` |
| | `Shutdown` | Restores every live editor's color, then `RestoreAll` |
| `Services/TargetWindowStyler.cs` | `Reassert(window)` | Re-extend margins; re-set Acrylic only if it changed |
| `Native/NativeMethods.cs` | `EmSetBkgndColor`, `ColorWindow`, `SmtoAbortIfHung`, `EnumChildWindows`, `SendMessageTimeout`, `GetSysColor` | P/Invoke additions |

## Hook changes not in the earlier sections

The `xaml.cpp` and `dllmain.cpp` descriptions above predate two changes in `cc29075`: the per-thread `transparentByThread` brush map, and the `WM_NCDESTROY` handling in `RootSubclass` with `RemoveGlass(bool destroying)`.

## Lessons that generalize

- **The alpha-0 rule applies to any app with an extended frame,** not just hooked ones. Any opaque-looking color an app paints with GDI-style alpha 0 is added onto the glass as a haze; black is the only color that turns into clean glass. Setting the app's own background to black (when it exposes a way, as `EM_SETBKGNDCOLOR` does) needs no injection.
- **Apps reset what you set from outside.** Explorer re-applies its backdrop and margins (handled inside the hook by detouring the DWM calls); Notepad re-calls `DwmExtendFrameIntoClientArea` and re-themes its text control. From outside the process, the answer is re-applying on activation and resize, plus a short burst afterwards, because the app's reset often lands after the event that reported it.
- **Set-and-restore is a read.** `EM_SETBKGNDCOLOR` has no getter, but it returns the previous value, so setting a sentinel and immediately putting the old value back reads the current color. Testing this way, without injection, found the cause in minutes.
- **Survey multi-window and multi-process cases early.** Both Explorer bugs only appear with more than one window in one process.

## How to close, relaunch and rebuild

- **Exit:** tray icon → Exit, or `[System.Threading.EventWaitHandle]::OpenExisting('Local\GlassForge.ExitRequest').Set()`. Closing the main window only hides it to the tray. Exiting restores Explorer windows and Notepad text colors.
- **Relaunch:** `C:\glass\GlassForge\src\GlassForge.App\bin\Debug\net8.0-windows10.0.19041.0\GlassForge.exe`
- **Rebuild:** exit first (the exe is locked), then `cmake --build C:\glass\GlassForge\native\build --config Release` (only if `native\hook` changed) and `dotnet build C:\glass\GlassForge\src\GlassForge.App -c Debug --source https://api.nuget.org/v3/index.json`.
- **Toggle Explorer glass:** `explorer` profile → Full File Explorer glass → Save & apply.

## Status of the earlier next steps

| Item | Status |
| --- | --- |
| Two windows in one process, then the shell process | Done: verified in the shell process with two windows |
| Toggle via the UI checkbox + Save & Apply | Not yet verified (only exit/relaunch tested) |
| Resize, navigation, tab switching, focus, Explorer restart while hooked | Partly: focus and window close/reopen exercised; Explorer restart not tested |
| Publish build with the native DLL embedded | Not done |
| README: name the one opt-in injection feature | Not done; deferred while the branch stays local |
| `DrawTextWithGlow` hook | Not done |
| Commit to `feat/hybrid-backdrop` | Done (5 local commits above); not pushed |

New open items:

- [ ] Watch Notepad over a longer session ("okay for now"): note any remaining black flash after focus or resize; the settle burst covers 5 s.
- [ ] Decide whether other apps that draw their own title bar (Windows Terminal: `WindowsTerminal`) join `OwnTitleBarApps`; untested.
- [ ] Light mode: Notepad's light-theme text color and the restore fallback (`#272727` is the dark default) are untested.
- [ ] The Claude Doc version of this writeup doesn't include this update yet.

---

# Update 2: Terminal, GlassForge's own look, app icons, Settings (Oct 1, 2026, evening)

In short: **Windows Terminal** gets glass on its tab row only, with no flashing; **GlassForge's own window** is glass with readable text; profiles and the running-apps list show **real app icons**. **Settings works from GlassForge** (`832a571`): the user applied the Settings profile and confirmed the look ("looks amazing"); the hook attached from GlassForge at 19:10:15. The commit is still labeled WIP from before that was confirmed.

## Commits since the last update (local only, not pushed)

| Commit | What |
| --- | --- |
| `832a571` | WIP: live glass for Windows Settings (works via test loader only) |
| `944150d` | Show real app icons instead of letter tiles |
| `4ef2bcf` | Brighten secondary text further over glass |
| `89f5282` | Give GlassForge's own window the glass look |
| `b2ec689` | Add a frame-only hook mode for Windows Terminal's tab row (also: focus-change backdrop guard, caption-button fix) |

Nothing is left uncommitted except the two personal screenshots in `docs/Images`, untracked on purpose.

## Windows Terminal: glass tab row

**Goal (user):** keep Terminal's own acrylic body exactly as configured; only the tab row and border should get GlassForge's live glass.

### What was tried, in order

| Attempt | Result |
| --- | --- |
| Terminal's `useAcrylicInTabRow` | Glitchy, flashes when switching tabs |
| Custom theme `GlassForge glass` (transparent `tabRow`, `window.useMica: true`) + GlassForge Native profile forcing Acrylic | Tab row glass, but grey flash on every focus change: Terminal re-applies Mica (2) on focus changes and GlassForge flipped it back 0.4–2 s later (measured by polling `DWMWA_SYSTEMBACKDROP_TYPE`) |
| Theme with `useMica: false` | Terminal then sets backdrop None (1) on every focus change instead: "blur to clear" flash |
| Fast guard in GlassForge (below) | Flip shortened to 15–30 ms, but without Extend the tab row is clear, not blurred: DWM draws the backdrop only in the frame, and Terminal's tab row is client area |
| Native profile with Extend into the whole window | Tab row blurred, but the whole window flashed on focus (Terminal resets margins) and it changed the body. Ruled out by the user |
| **Frame-only hook (shipped)** | Tab row blurred, no flash, body untouched |

### Focus-change guard (no injection, `b2ec689`)

`NativeBackdropSession` now listens to `EVENT_SYSTEM_FOREGROUND` for every focus change, not only the styled window gaining focus (an app resets its backdrop on losing focus too), and runs `_guardTimer` (15 ms, 1.5 s) calling `TargetWindowStyler.ReassertBackdrop`: read `DWMWA_SYSTEMBACKDROP_TYPE`, write Acrylic only if it changed. `Reassert` (backdrop + margins) runs once per focus change; the 5 s settle burst only when the styled window itself gains focus (it also re-sends Notepad's text color, which repaints).

### Frame-only hook mode (`b2ec689`)

The opt-in hook (`AppProfile.HookGlass`, still serialized as `"ExplorerGlass"` via `[JsonPropertyName]` so existing Explorer profiles keep working) now also attaches to Terminal (`HookWindowClass` = `CASCADIA_HOSTING_WINDOW_CLASS`) in a **frame-only** mode (`t_frameOnly` in `dllmain.cpp`):

- `IsTargetDC` returns false: no paint changes, no XAML connection (so Windows unloads the DLL fully on unhook).
- `GlassMargins`: `{0, 0, MulDiv(40, dpi, 96), 0}`, glass behind the top 40 DIPs only (tab row + border). The hook logs Terminal's own `DwmExtendFrameIntoClientArea` requests (`frame request …: 0 0 0 0`) for tuning.
- The existing DWM detours block Terminal's backdrop/margin resets; `WM_NCACTIVATE` stays TRUE (glass when unfocused).
- Terminal theme used with it: `"theme": "GlassForge glass"` with `tabRow.background`/`unfocusedBackground` `#00000000`, `window.useMica: false`, `useAcrylicInTabRow: false`; profiles keep `useAcrylic: true` (the user's body). Backup: `settings.json.before-glassforge.bak` in Terminal's `LocalState`.
- UI: "Glass tab row (EXPERIMENTAL)" on `WindowsTerminal` profiles, with a warning that every Terminal tab shares one process.

### Doubled caption buttons (`b2ec689`)

With glass extended over the title area, DWM draws its native minimize/maximize/close under the app's own, slightly offset (seen in both Terminal and GlassForge). DWM only draws them for windows with `WS_SYSMENU`, confirmed by removing the style from outside. Fix: the Terminal hook drops `WS_SYSMENU` while glass is on (`SetSysMenu`, restored on detach), and `WindowBackdropService.HideNativeCaptionButtons` does the same for GlassForge's window (re-applied on `StateChanged`). Trade-off: Alt+Space shows no system menu while active.

## GlassForge's own window (`89f5282`, `4ef2bcf`)

- `PageBrush` tint over the window's Acrylic: `#DC0C0C10` (86%) → `#400C0C10` (25%).
- `WindowBackdropService.KeepGlassWhenInactive`: an `HwndSource` hook answers `WM_NCACTIVATE(FALSE)` with `DefWindowProc(…, TRUE, …)` so the glass doesn't turn flat when GlassForge loses focus.
- Secondary text brightened for glass: `MutedBrush` `#9C99A8` → `#E6E4EE`, `SubtleBrush` `#6C6977` → `#C9C6D4`.
- A larger UI clean-up (simplified layout / Windows Settings style / tray-first) was offered; not chosen yet.

## Real app icons (`944150d`)

`Services/AppIconService.cs`, cached by path:

1. Packaged apps (a process with a package identity, via `GetApplicationUserModelId`): the Start menu icon from `shell:AppsFolder\<AUMID>` through `IShellItemImageFactory.GetImage` (48 px, premultiplied BGRA read with `GetDIBits` into a `Pbgra32` `BitmapSource`).
2. Otherwise the exe's own icon: `SHDefExtractIcon(path, 0, …, 48)`.
3. Otherwise the window's icon (`WM_GETICON` via `SendMessageTimeout`, then `GCLP_HICON`).
4. Otherwise the letter tile.

UWP apps hosted by ApplicationFrameHost (Settings): the frame window carries no app identity; `ForHostedApp` finds the hosted `Windows.UI.Core.CoreWindow` child, whose process is the real app. Profiles store `ExecutablePath` (saved in JSON) so icons show while the app is closed. XAML: `AppIcon`/`LetterTile` styles switch on `HasIcon`. `RunningApplication` keeps no cached fields (records compare every field); caching lives in the service.

## Windows Settings: working

### Findings

| Question | Answer |
| --- | --- |
| Why does Native backdrop show nothing? | Settings is UWP: its window frame belongs to `ApplicationFrameHost.exe`, and the visible layer is **Settings' own Mica**, a brush WinUI 2's `BackdropMaterial` puts in the XAML window's system backdrop slot (`ICompositionSupportsSystemBackdrop` on `Windows.UI.Xaml.Window.Current`). It covers the frame's DWM backdrop (which read Acrylic the whole time). "Static wallpaper tint" = Mica |
| Is Settings sandboxed? | No: `SystemSettings.exe` is medium integrity, not AppContainer, like Notepad/Terminal |
| XAML framework | System XAML (`Windows.UI.Xaml.dll`) + WinUI 2; diagnostics via `InitializeXamlDiagnosticsEx` from `Windows.UI.Xaml.dll`, connection names `VisualDiagConnectionN` (same pattern as Windhawk's Start menu styler) |
| Settings' XAML page | Fully transparent (`RootPage`, `ContentGrid`, `AppTitleBar`, `PaneRoot` all clear); survey: 1,082 elements, 358 brushes |
| Empty the backdrop slot? | Black: ApplicationFrameHost draws nothing there, and the DWM backdrop doesn't reach through |
| **Fill the slot with a host backdrop brush** | **Live glass** (`ICompositor3::CreateHostBackdropBrush` from the XAML window's compositor, with `DWMWA_USE_HOSTBACKDROPBRUSH` on the frame) |
| Grey when unfocused | Settings swaps in a flat fallback brush when inactive. Fixed by redirecting its `put_SystemBackdrop` (method slot 7 of the interface): every request is remembered for restore and the glass brush stays. User confirmed: "stays glass" |

### Code (`832a571`)

- `native/hook/uwp.cpp`: `ApplyUwpGlass(frame)`, `RestoreUwpBackdrop()` (undoes the redirect first, then gives back the app's last brush), `UndoUwpRedirect()` (also called from `DllMain` on unload).
- `dllmain.cpp`: third hook mode `t_xamlOnly` for an `ApplicationFrameWindow` root inside `SystemSettings.exe`; no paint changes; the frame belongs to another process, so no subclass, and a thread timer (`t_threadTimer`, 1 s) drives the re-check; `OwnsWindow` guards subclassing.
- `xaml.cpp`/`xaml.h`: `XamlStyle { app, logAll }` per thread; system-XAML connection fallback; survey logging of brushes with their color (`LogBackground`, `BrushColor`).
- App side: `AppProfile.IsSettings`, `UwpContentWindowClass`; `ExplorerHookSession(windowClass, processName)` finds `CoreWindow`s inside frames and filters by process; running-apps list resolves ApplicationFrameHost entries to the hosted app; "Glass background (EXPERIMENTAL)" toggle with warning; profile `ApplicationFrameHost` replaced by `SystemSettings` (backup `profiles.json.before-settings.bak`).

### Status

Working from GlassForge. The session attached on its own (`glass applied`, `UWP backdrop calls redirected`, `UWP backdrop replaced 00000000` at 19:10:15), and the user confirmed the look ("looks amazing"). The profile is saved in `profiles.json` (19:10:34). An earlier check right after launch showed no attach. The session sweeps every 3 s, and the likely reason is that Settings' content window wasn't attached to its frame at that moment (a minimized UWP app's `CoreWindow` is detached). Next steps:

- [ ] Verify: Stop/disable restores Mica, minimize/restore, closing and reopening Settings, theme changes.
- [ ] Confirm the attach happens promptly when GlassForge starts with Settings already open.
- [ ] The `832a571` message still lists the attach as a known issue; note the correction in the next commit message (history isn't rewritten).

## Open issue: profiles disappearing

Twice a profile vanished from `profiles.json` without being deleted on purpose (Outlook `olk`, later the Windows Terminal profile). GlassForge only saves on add, delete and Save & apply; the likely cause is a GlassForge window with an older in-memory list saving over a file that was edited outside the app. Not yet confirmed or fixed.

---

# Update 3: Task Manager (Oct 1, 2026, night): parked

**State:** measured and probed only. Nothing was added to the app. The user accepted the look and parked it, so the next app is Microsoft Word. The final probe is `native/tools/taskmgr-probe.ps1`.

## What Task Manager is

| | |
| --- | --- |
| Exe | `C:\Windows\System32\Taskmgr.exe` (10.0.28000.2738) |
| Manifest | `requestedExecutionLevel asInvoker` plus `autoElevate true`. For an admin user with UAC on (`ConsentPromptBehaviorAdmin = 5`), a normal launch silently elevates |
| **Integrity** | **High (`0x3000`, elevated).** GlassForge, Terminal and Claude Code are Medium (`0x2000`) |
| Main window | `TaskManagerWindow`, style `0x14CF0000`. Asks DWM for Mica (`DWMWA_SYSTEMBACKDROP_TYPE = 2`), dark mode on |
| Children | 6x `Windows.UI.Composition.DesktopWindowContentBridge` (system XAML islands, like Settings' XAML, **not** WinUI 3); `NativeHWNDHost` → `DirectUIHWND` holding the Details page (`SysListView32`, `SysHeader32`, `SysTabControl32`, `ScrollBar`s); 3x `GlassWindow` / `FilterControlGlassWindow` (search box and toolbar); a hidden `CoreWindow` |

The closest earlier case is Notepad: a Win32 window with a DWM backdrop behind XAML that is already clear.

## What Windows blocks from Medium integrity (UIPI)

All tested against the live window. None of them changed anything.

| Call | Result |
| --- | --- |
| `DwmSetWindowAttribute` (Native mode) | `0x80070006` (E_HANDLE) |
| `SetWindowLongPtr(GWL_EXSTYLE)` (Host mode layering) | Error 5 |
| `PostMessage` (registered message; also `WM_NCACTIVATE`) | Error 5 |
| `SetWindowsHookEx` (opt-in hook) | Never loads into a higher-integrity process (documented UIPI behavior; not tested) |

**No GlassForge mode works on elevated Task Manager while GlassForge runs as a normal user.** Reading (`DwmGetWindowAttribute`, `GetClassName`, `OpenProcess` with query-limited access) works.

## Probes (elevated, one UAC prompt each, user's go-ahead)

| Probe | What | User's verdict |
| --- | --- | --- |
| 1 | Acrylic (3) + margins `{-1,-1,-1,-1}`, re-applied every 100 ms | **Processes list is glass** (XAML pages clear, same as Notepad). Task Manager never reset its backdrop (0 resets logged). Goes flat when unfocused |
| 2 | + `WM_NCACTIVATE(TRUE)` posted when focus leaves (100 ms poll) + Details `SysListView32` background set to black (`LVM_SETBKCOLOR`; the original was `#191919`, text background `CLR_NONE`) | Glass kept when unfocused, but **flashes** on focus-out (the poll gap). **Details stays solid grey**: the `ListView` background color isn't what paints it; DirectUI/uxtheme draws it, so it would need the paint hooks Explorer uses. The user is OK with grey Details |
| 3 (`taskmgr-probe.ps1`) | Same, but `WM_NCACTIVATE(TRUE)` posted from a `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)` callback, so within milliseconds | **Untested by eye**: parked before the user judged the flash |

All probes restored Mica, zero margins and the ListView colors when stopped (`restored … hr=0x0`).

## Options if it's picked up again

1. **Opt-in elevated helper (recommended):** a GlassForge mode that runs elevated after one UAC prompt, only while a profile targets an elevated window, and does only the Native work (backdrop, margins, keep-active). It needs an EXPERIMENTAL toggle and a red warning, because it breaks the README's "no admin" promise. It also covers any other elevated app.
2. **Launch Task Manager at Medium** (`__COMPAT_LAYER=RunAsInvoker`): plain Native mode works with no admin, but Task Manager loses its admin powers, and it doesn't apply to Ctrl+Shift+Esc.
3. **Injection** (the only route to glass on Details and to a fully flash-free unfocus) would also need elevation. Not proposed.

## Untested

- Whether probe 3 removes the focus-out flash, or only shortens it (an outside process always lands after `WM_NCACTIVATE(FALSE)`)
- Light mode, Performance/App history/Startup/Users/Services pages, resize and maximize, and Task Manager's "always on top"
- The extended-frame alpha-0 effect on the `GlassWindow` search/toolbar strips (no haze reported)

---

# Update 4: Microsoft Word (Oct 1, 2026, night)

**State:** working and confirmed by eye ("looks great", top strip included). It's an opt-in hook toggle, "Glass background (EXPERIMENTAL)", on `WINWORD` profiles. Word needs closing and reopening after the toggle is turned on or off.

## What Word is

| | |
| --- | --- |
| Exe | `C:\Program Files\Microsoft Office\root\Office16\WINWORD.EXE` 16.0.20326 (Microsoft 365, Click-to-Run) |
| Integrity | Medium, same as GlassForge |
| Main window | `OpusApp` (Win32). Children: `MsoCommandBarDock` → `MsoCommandBar` → `MsoWorkPane` → `NUIPane` → `NetUIHWND` (ribbon, status bar), `_WwF` → `_WwB` → `_WwG` (page), `NUIScrollbar` |
| Rendering | Office's own UI (NetUI) in a Windows.UI.Composition layer (`dcomp.dll`, `wuceffects.dll`). The GDI bitmaps Office blits into the window are empty (`00000000`) |
| DWM | `DWMWA_SYSTEMBACKDROP_TYPE` 0, no accent policy; Office sets caption, text and border colours (attributes 35/36/34) from `mso98win32client.dll` |
| Mica | Office's own: `mso40uiwin32client.dll` calls `Compositor::TryCreateBlurredWallpaperBackdropBrush` (a blur of the **static** wallpaper image) and feeds it into an effect graph `Blend(Blend(param 'BlurredWallpaperBackdrop', Opacity(Flood)), Opacity(Flood))` |

## What didn't work, in order

| Attempt | Result |
| --- | --- |
| Native mode (Acrylic + whole-window extend, no injection) | Nothing visible: Office's surface is opaque |
| Hooking an already-open Word and opening a new window | No brush calls: Office made its brushes at startup |
| Hooking from launch (`native/tools/launch-hooked.ps1`) and swapping the brush | Sides live, top strip still static brown |
| Red diagnostic on the graph's wallpaper input | Only the sides turned red |
| DWM backdrop None/Acrylic, wallpaper file opens (kernel32 and ntdll), Direct2D/GDI gradients and solid colours, accent colour, bitmap uploads | None explained the strip. Moving the window showed it identical everywhere |
| GlassForge's Native session plus the hook | No brush requests at all: the hook DLL loads into Word ~0.4 s before the UI thread runs hook code, and Office makes its brushes in that gap |

**The actual cause of the static top strip:** the same wallpaper brush, requested in that startup gap before the detour existed. Every earlier "the strip is something else" result was really "the strip's brush was made before we hooked". Once the detour was installed from `DllMain` (a thread started at DLL load), the strip went live as well.

Disassembly note (`dcomp.dll`): the public brush methods share one constructor (`RVA 0xADB94`) with a type argument, **0 = backdrop, 1 = host backdrop, 2 = blurred wallpaper**.

## Final approach (`native/hook/office.cpp`)

- **Early detour:** `DllMain` (process `WINWORD.EXE`) starts `StartOfficeHooksEarly`, a thread that finds the method on a throwaway Compositor (own `DispatcherQueue`) and detours `TryCreateBlurredWallpaperBackdropBrush`. Logged: detour in ~10 ms after load, ahead of all of Office's requests.
- **Swap:** each request makes Office's brush *and* `ICompositor3::CreateHostBackdropBrush`, and returns the host one. Office's tint graph stays on top, so the glass is darker than Notepad's.
- **Opt-in:** `DWMWA_USE_HOSTBACKDROPBRUSH` on `OpusApp`.
- **Focus out went black:** `WM_NCACTIVATE` alone and `WM_ACTIVATEAPP(FALSE)` alone didn't help; swallowing `WM_ACTIVATE(WA_INACTIVE)` did (kept with the other two). The subclass is removed on Detach, which also sends the inactive messages again.
- **Safety:** the DLL is pinned in Word once installed (subclass and patches must never outlive it).
- **App side:** `AppProfile.IsWord`, `HookWindowClass = AnyWindowClass ("*")`, so the session hooks any top-level Word window, splash screen included. `GlassHostManager` runs **only** the hook session for Word: no Native session, because there's nothing for DWM attributes to show through. UI: "Glass background (EXPERIMENTAL)" with a red warning (every document shares one process; unsaved work can be lost).

## Not working / untested

- **Turning it off doesn't restore a running Word.** `office.cpp` records Office's effect inputs through `SetSourceParameter` (via the effect factory) to swap the brushes back live, but the first test recorded none (`0 of 0 inputs switched`), so the UI says to close and reopen Word. Next step: find where the brushes go (the earlier survey also saw `SetSourceParameter` only for the second brush of each pair).
- A Word opened before GlassForge needs restarting once.
- Native backdrop alone does nothing for Word, as the UI says.
- Untested: other Office apps (Excel, PowerPoint, Outlook classic use the same `mso40uiwin32client.dll` and probably the same fix), light theme, multiple Word windows, Word crash recovery, and long sessions.
- Tool: `native/tools/launch-hooked.ps1` launches an exe and hooks every UI thread from its first window (startup surveys).

---

# Update 5: Excel and PowerPoint (started, Oct 1, 2026, night)

**State:** measured only; no code yet. The handoff prompt for the next session is `docs/NEXT_AGENT_PROMPT.md`.

| | Excel | PowerPoint |
| --- | --- | --- |
| Exe | `Office16\EXCEL.EXE` | `Office16\POWERPNT.EXE` |
| Integrity | Medium | not measured |
| Main window class | `XLMAIN` | not measured (probably `PPTFrameClass`) |
| DWM backdrop | 0 | not measured |
| Office UI library | `mso40uiwin32client.dll`, `dcomp.dll`, `wuceffects.dll` loaded, same as Word | not measured |

The first PowerPoint launch showed no process (cause unknown). Plan: generalise Word's `WINWORD.EXE`/`OpusApp` checks
(hook DLL) and `IsWord` (app) to an Office list, then test each app from a fresh launch through GlassForge.
