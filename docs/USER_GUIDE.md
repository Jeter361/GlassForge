# GlassForge user guide

## What GlassForge does

GlassForge adds a configurable frosted-glass appearance to ordinary Windows applications. It does not modify the target application's files. Instead, it:

1. Finds a visible window owned by the selected process.
2. Makes that application window partially transparent.
3. Creates a separate acrylic backdrop directly behind it.
4. Tracks movement, resizing, focus changes, and DPI scaling.

The application content remains on the foreground window, so text, icons, and controls stay sharp. Only the material behind that foreground is blurred.

## Starting GlassForge

During development, run the release executable:

```powershell
C:\glass\GlassForge\src\GlassForge.App\bin\Release\net8.0-windows\GlassForge.exe
```

Or start it from source:

```powershell
cd C:\glass\GlassForge
dotnet run --project src\GlassForge.App
```

To inspect the interface without applying enabled profiles:

```powershell
dotnet run --project src\GlassForge.App -- --no-effects
```

Closing the main window minimizes GlassForge to the notification area. Use the tray icon to reopen or fully exit it.

## Applying glass to an application

1. Open the application you want to customize.
2. Open GlassForge.
3. Find the application in the live **Running applications** browser. The list updates automatically.
4. Use the search field if needed.
5. Select **Add** on the application's card.
6. Review the generated display name and process name.
7. Adjust opacity, tint strength, and tint color.
8. Keep **Enabled** checked.
9. Select **Save & apply**.

Profiles target owning process IDs, not title text. A document containing the word “Teams,” for example, cannot accidentally receive the Teams profile.

## Settings

### Window opacity

Controls the opacity of the original application window.

- Higher values preserve more foreground contrast.
- Lower values reveal more acrylic material.
- The default is `195/255`, approximately 76%.

Lower values also affect foreground content because Windows applies opacity to the complete target window. Keep this high enough for readable text.

### Tint strength

Controls how strongly the acrylic host is colored.

- `0` allows the most natural wallpaper color.
- Higher values produce a more deliberate colored-glass surface.
- The neutral default is `32/255`.

### Tint color

A six-digit RGB hexadecimal color without an alpha component.

- Neutral charcoal: `181818`
- Cool blue: `19324A`
- Violet: `7200B8`
- Warm brown: `3A251B`

Wallpaper colors remain most natural with a dark neutral color and low tint strength.

## Managing profiles

- **Save & apply** saves the complete profile list and restarts the selected effect.
- **Stop** removes the selected effect and restores the target window's original extended style.
- **Delete** stops and removes the selected profile.
- **Enabled** controls whether the profile starts automatically with GlassForge.
- **Start with Windows** registers GlassForge under the current user's Windows Run key with `--background`.

Profiles are stored here:

```text
%LOCALAPPDATA%\GlassForge\profiles.json
```

## Built-in starting profiles

The first launch creates profiles for:

| Application | Process |
|---|---|
| Microsoft Teams | `ms-teams` |
| New Microsoft Outlook | `olk` |
| Microsoft Word | `WINWORD` |

These are ordinary editable profiles, not hard-coded special cases.

## Troubleshooting

### The application is not listed

Make sure it has a visible top-level window and wait up to two seconds for live discovery. Background-only and tray-only processes are intentionally excluded.

### Color customization

Use a built-in swatch, enter a six-digit hex color, adjust the individual red/green/blue channels, or open the full Windows color dialog. The preview updates immediately; the target application changes only after **Save & apply**.

### The effect does not appear

- Confirm the process name is correct.
- Confirm the application is running at the same privilege level as GlassForge.
- Select **Stop**, then **Save & apply**.
- Some protected, elevated, or special compositor windows cannot be controlled from a normal user process.

### A window stays transparent after a crash

Exit the target application and reopen it, or run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\glass\ResetGlassWindows.ps1
```

Normal profile shutdown restores the original style automatically. The reset script is a development recovery tool for interrupted prototypes or crashes.

### The backdrop trails during fast dragging

GlassForge uses a separate backdrop HWND. Location events and synchronous window coordinates minimize delay, but Windows can still compose the two HWNDs one frame apart during rapid movement. This is a known architectural limitation of the compatibility backend.

## Safe testing workflow

1. Ensure the old `.vbs` prototype launchers remain disabled.
2. Run GlassForge with `--no-effects` and inspect the UI.
3. Enable one profile at a time.
4. Test move, resize, minimize, restore, focus switching, multiple monitors, and display scaling.
5. Use **Stop** before exiting the target application during early testing.
