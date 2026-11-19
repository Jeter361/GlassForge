#pragma once
#include <windows.h>

// Experimental (docs/GLASS_WRITEUP.md, Update 7): live host-backdrop glass behind File Explorer's content, like Teams
// and Settings. Off unless %LOCALAPPDATA%\GlassForge\explorer-hostglass.txt exists. Returns true when the config also
// asks for DWM's own Acrylic to be turned off (the caller owns the real DwmSetWindowAttribute).
bool ApplyExplorerHostGlass(HWND root);
void RemoveExplorerHostGlass(HWND root);
