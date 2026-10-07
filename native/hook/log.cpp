#include <windows.h>
#include <shlobj.h>
#include <cstdarg>
#include <cstdio>
#include "log.h"

#pragma comment(lib, "shell32.lib")

namespace
{
    SRWLOCK g_lock = SRWLOCK_INIT;

    const wchar_t* LogPath()
    {
        static wchar_t path[MAX_PATH] = L"";
        if (path[0] == L'\0')
        {
            PWSTR base = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)))
            {
                swprintf_s(path, L"%s\\GlassForge\\hook.log", base);
                CoTaskMemFree(base);
            }
        }
        return path;
    }
}

void Log(const wchar_t* format, ...)
{
    wchar_t message[1024];
    va_list args;
    va_start(args, format);
    vswprintf_s(message, format, args);
    va_end(args);

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* name = wcsrchr(exe, L'\\');
    SYSTEMTIME time;
    GetLocalTime(&time);

    // One append handle per process, opened on the first line ("loaded", from DllMain). A browser's GPU process locks
    // itself down after startup and can't open files any more, but a handle it already holds keeps working.
    static HANDLE file = INVALID_HANDLE_VALUE;
    wchar_t line[1200];
    const int length = swprintf_s(line, L"%02d:%02d:%02d.%03d %s[%lu] %s\n", time.wHour, time.wMinute, time.wSecond,
        time.wMilliseconds, name ? name + 1 : exe, GetCurrentProcessId(), message);
    char utf8[3600];
    const int bytes = length > 0 ? WideCharToMultiByte(CP_UTF8, 0, line, length, utf8, sizeof utf8, nullptr, nullptr) : 0;
    AcquireSRWLockExclusive(&g_lock);
    if (file == INVALID_HANDLE_VALUE)
        file = CreateFileW(LogPath(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    if (file != INVALID_HANDLE_VALUE && bytes > 0) WriteFile(file, utf8, bytes, &written, nullptr);
    else if (bytes > 0)
    {
        // Sandboxed from the start (a Chromium GPU process): no file, but a section a listener created can be opened
        // (as the keep-solid areas are). Layout: LONG used, then the text. Only there while someone is listening.
        static char* shared = nullptr;
        static bool tried = false;
        if (!tried)
        {
            tried = true;
            if (HANDLE section = OpenFileMappingW(FILE_MAP_WRITE, FALSE, L"Local\\GlassForge.HookLog"))
                shared = static_cast<char*>(MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 0));
        }
        constexpr LONG capacity = 4 * 1024 * 1024;
        if (shared)
        {
            const LONG at = InterlockedExchangeAdd(reinterpret_cast<volatile LONG*>(shared), bytes);
            if (at + bytes <= capacity) memcpy(shared + sizeof(LONG) + at, utf8, bytes);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}
