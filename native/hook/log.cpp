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

    AcquireSRWLockExclusive(&g_lock);
    FILE* file = nullptr;
    if (_wfopen_s(&file, LogPath(), L"a, ccs=UTF-8") == 0 && file)
    {
        fwprintf(file, L"%02d:%02d:%02d.%03d %s[%lu] %s\n", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
            name ? name + 1 : exe, GetCurrentProcessId(), message);
        fclose(file);
    }
    ReleaseSRWLockExclusive(&g_lock);
}
