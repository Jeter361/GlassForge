# Session notes: October 6, 2026

Handoff for the next session. Branch `feat/minimal-ui`; everything up to `52e2422` is pushed, and the commits after
that are local only (see "Where things stand").

## Headline

"Glass inside the app" now works on apps that were dead ends: they paint their whole window themselves, so Native
glass showed nothing. GlassForge turns the app's own background color into live glass and keeps text, pictures and
video solid. It works on LogMeIn Resolve, ChatGPT and the new Outlook.

## Results by app

| App | Kind | Status |
| --- | --- | --- |
| LogMeIn Resolve | Qt | Works. Remote screen stays solid, its letterbox bars are glass, full screen gets no glass. |
| ChatGPT | Store app, Electron | Works and looks good. Pictures stay solid; a dark-border line was softened. |
| New Outlook | Store app, WebView2 | Works after several stability fixes; looked clean when hovered. Needs a longer real-use check. |
| Teams | Store app, WebView2 | Not tried yet. Same kind of app as Outlook, so it should follow. |
| Notepad | WinUI | New "Solid text area (HDR)" option fixes invisible text when HDR is on. |

## What the user sees in GlassForge

- **New minimal editor.** One Apps list with on/off switches and real app names. "+ Add" opens a picker. Changes apply
  live, so there's no Save button. Rarely used options sit under Advanced.
- **Glass inside the app (experimental).** A per-app toggle. Turning it on or off restarts the app, so anything open in
  it (for example a LogMeIn remote session) is lost.
- **Diagnose this app.** A read-only check: what the app is built with, how it draws, whether it's a Store app or runs
  as admin. It recommends a glass mode, and Apply sets it.

## Lessons learned (what broke and the fix, in plain terms)

| Symptom | Cause | Fix |
| --- | --- | --- |
| Notepad text invisible | HDR handles see-through pixels differently | HDR toggle that keeps the text area solid |
| Black instead of glass at first | Windows ignores transparency on the app's normal drawing surface | Draw through a surface type that respects it |
| Remote screen / photos see-through | Everything near the background color turned clear | Find pictures and video on the page and keep them solid |
| Black bars around the remote screen | Darker-than-background areas can't be cleared by color alone | Trim the bars and draw them as plain glass |
| White box after growing a window | Unpainted area under the glass defaults to white | Paint it black, which reads as clear |
| Washed-out light glass | The glass came up in light mode | Force dark mode; keep GlassForge's profile doing the frame |
| Flash on clicking in and out | The window looked inactive for a moment | Keep it drawn as active |
| Full-screen remote view broke | The page stops reporting its layout after full screen | Remember where the remote screen sits in its page |
| Stream felt slower | Too much checking per frame and per second | Lighter checks, no stalls, less polling |
| ChatGPT dark line on a toggle | A subtle darker border became a hard line | Darker-than-background shows at half strength |
| Outlook text eroding on hover | Partial redraws were processed twice | Only process what actually changed |
| Outlook look kept changing | It re-picked its main layer and background while in use | Lock both once settled |
| Gray tint over the whole desktop (once) | Unclear; happened after force-restarting ChatGPT's graphics process | Don't do that again; the Store-app route avoids it |

**Rejected, don't redo:**
- Making extra flat colors clear (for LogMeIn's black title bar and sidebar): too glitchy, reverted.
- A heavier 55% tint: the user prefers 12%.
- Having the hook set the glass itself without GlassForge's profile: caused a light wash.

## Where things stand

- `feat/minimal-ui` is pushed up to `52e2422`.
- **Local only, not pushed yet:**
  - WebView2/Outlook support
  - only the window's main layer gets glass; other layers stay solid
  - partial redraws processed once
  - locking Outlook's main layer and background once settled
  - the Teams-related change in how the glass reaches WebView2 apps

  Push them once Outlook has been used for a while without problems.
- No release has been built from this branch; the last pre-release is v0.2.0-beta.1.
- **Profiles:** LogMeIn, ChatGPT and Microsoft Outlook have "Glass inside the app" on. The LogMeIn profile may be
  switched off (it didn't come back on the last restart).
- **Dev settings to know about:** Outlook and ChatGPT have Windows' package launch setting enabled for the glass.
  `native/tools/package-glass.ps1 -Package <name> -Off` turns it off.
- **Backups** in `%LOCALAPPDATA%\GlassForge`: `profiles.json.before-outlook.bak` and `profiles.json.before-dxgi-toggle.bak`.

## Known issues / next steps

1. **Outlook:** use it normally for a while (open, scroll, compose, menus). If it holds, push the local commits.
2. **Pictures in Outlook and Teams:** their positions don't line up yet, so email images may go gray.
3. **Hook updates:** after the hook is rebuilt, GlassForge doesn't refresh the Store apps' launch setting. It currently
   needs a manual reset (turn the setting off, restart the app, let GlassForge relaunch it).
4. **Teams:** try it, carefully. Restarting Teams drops calls.
5. **LogMeIn:** a brief see-through flash can still happen when a session first opens.
6. **Diagnosis:** label fixes from today are in; add an automatic diagnosis when an app is added.
7. **Release:** a v0.3.0-beta.1 pre-release once Outlook is confirmed.
8. **Housekeeping:** an empty Claude Doc ("GlassForge: Universal Native Glass") was started and abandoned; delete it if
   you don't want it. `docs/GLASS_PLAYBOOK.md` and two personal screenshots in `docs/Images` remain untracked on purpose.
