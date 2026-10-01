#pragma once

// Appends one line to %LOCALAPPDATA%\GlassForge\hook.log, prefixed with time and process. Safe from any thread.
void Log(const wchar_t* format, ...);
