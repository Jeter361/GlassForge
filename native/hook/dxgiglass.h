#pragma once
#include <windows.h>

// Experimental "glass for any app" path for apps that present with DXGI (Chromium/Electron, Qt Quick/RHI, Flutter):
// every frame, the app's own background color is turned into transparency (color-to-alpha) just before Present, so
// the DWM backdrop behind the window shows through while text and controls stay solid. See dxgiglass.cpp.

// True when %LOCALAPPDATA%\GlassForge\dxgi.txt lists this process's exe name (experiment switch).
bool DxgiGlassRequested();
// Detours IDXGISwapChain::Present/Present1 for the whole process. Safe to call more than once.
void InstallDxgiGlass();
// Same, from DllMain: on a thread of its own, so it is in place before the app creates its swap chains.
void StartDxgiGlassEarly();
// Takes the detours out and frees the shader resources.
void RemoveDxgiGlass();
