#pragma once
#include <windows.h>
// Called on a UWP app's UI thread: swaps its system backdrop (WinUI 2 Mica) for a live host backdrop brush, keeping
// the app's brush to restore. Safe to call repeatedly; it only acts when the app has put its own brush back.
void ApplyUwpGlass(HWND frame);
// Called on the same thread: puts the app's own backdrop brush back.
void RestoreUwpBackdrop();
// Called from DllMain on unload: never leave the app's method table pointing into an unloaded DLL.
void UndoUwpRedirect();
