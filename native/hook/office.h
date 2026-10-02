#pragma once
#include <windows.h>

// Microsoft Office (Word, Excel, PowerPoint) glass; see office.cpp. All of these are process-wide and safe to repeat.
// Installs the detours (once) and turns the glass on, switching any inputs Office already has to live brushes.
void InstallOfficeHooks();
// From DllMain in an Office process (WINWORD/EXCEL/POWERPNT): InstallOfficeHooks on a new thread, without waiting.
void StartOfficeHooksEarly();
// Turns the glass off: Office's own brushes go back into every input; the detours stay in place.
void DisableOfficeGlass();
// Process exit only: takes the detours and vtable patches out.
void RemoveOfficeHooks();
// On the window's thread: lets the host backdrop brush render in an Office main window and keeps it drawn as active.
void OptInOfficeWindow(HWND window);
// On the window's thread: undoes OptInOfficeWindow for this thread's windows.
void ReleaseOfficeWindows();
