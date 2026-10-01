#pragma once
#include <windows.h>
enum class XamlApp { Explorer, Settings };
struct XamlStyle {
    XamlApp app = XamlApp::Explorer;
    // Survey: log every element on the thread and each non-empty Background/Fill, not just Explorer's own types.
    bool logAll = false;
};
// Called on a hooked window's thread: connects to the process's XAML diagnostics once (WinUI 3, or system XAML for
// UWP apps), then styles that thread's elements.
void AddXamlThread(HMODULE module, DWORD thread, const XamlStyle& style);
// Called on a hooked window's thread: gives back that thread's original backgrounds and stops styling it.
void RemoveXamlThread();
// Called once no hooked window is left: disconnects from the visual tree.
void StopXamlStyling();
// Called on a hooked window's thread from its timer: re-applies overrides Explorer rebuilt.
void RefreshXamlStyling();
