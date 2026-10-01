#pragma once
#include <windows.h>
// Called on a hooked window's thread: connects to the process's XAML diagnostics once, then styles that thread's elements.
void AddXamlThread(HMODULE module, DWORD thread);
// Called on a hooked window's thread: gives back that thread's original backgrounds and stops styling it.
void RemoveXamlThread();
// Called once no hooked window is left: disconnects from the visual tree.
void StopXamlStyling();
// Called on a hooked window's thread from its timer: re-applies overrides Explorer rebuilt.
void RefreshXamlStyling();
