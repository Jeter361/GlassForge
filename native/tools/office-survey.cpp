// REFERENCE ONLY, not built (CMakeLists.txt doesn't list it). The survey build of native/hook/office.cpp used on
// 2026-10-01 to find how Office draws its Mica: logs Compositor brush creation, effect graphs (SetSourceParameter),
// sprite brushes, DWM calls, wallpaper file opens (kernel32 + ntdll), Direct2D colours/gradients/bitmaps and GDI
// blits, plus the title-strip clearing experiment (ClearStrip). To use it: copy over native/hook/office.cpp, build,
// launch with native/tools/launch-hooked.ps1, read %LOCALAPPDATA%\GlassForge\hook.log. See GLASS_WRITEUP.md, Update 4.

// Microsoft Office (Word): Office draws its own Mica-like backdrop instead of asking DWM for one (the window's
// DWMWA_SYSTEMBACKDROP_TYPE stays 0 and its surface is opaque), and it loads Windows.UI.Composition. The guess is a
// Compositor brush made from the static wallpaper. This file is the survey: it detours the Compositor's backdrop
// brush factories and logs every call, changing nothing.
#include <windows.h>
#include <roapi.h>
#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>
#include <DispatcherQueue.h>
#include <windows.ui.composition.h>
#include <intrin.h>
#include <dwmapi.h>
#include <set>
#include <algorithm>
#include <shlwapi.h>
#include <string>
#include <winstring.h>
#include <windows.graphics.effects.interop.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include "detours.h"
#include "log.h"
#include "office.h"

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;
using namespace ABI::Windows::UI::Composition;

namespace {
using CreateBrush = HRESULT(STDMETHODCALLTYPE*)(void* self, ICompositionBackdropBrush** result);
// Vtable slots: IUnknown (3) + IInspectable (3), then the interface's methods in declaration order.
constexpr int BlurredWallpaperSlot = 6; // ICompositorWithBlurredWallpaperBackdropBrush::TryCreateBlurredWallpaperBackdropBrush
constexpr int BackdropSlot = 8;         // ICompositor2::CreateBackdropBrush (after CreateAmbientLight, CreateAnimationGroup)
constexpr int HostBackdropSlot = 6;     // ICompositor3::CreateHostBackdropBrush

CreateBrush Real_BlurredWallpaper = nullptr;
CreateBrush Real_Backdrop = nullptr;
CreateBrush Real_HostBackdrop = nullptr;
// Survey: how else Office reaches the static wallpaper (file reads and the wallpaper path query).
decltype(&CreateFileW) Real_CreateFileW = CreateFileW;
decltype(&SystemParametersInfoW) Real_SystemParametersInfoW = SystemParametersInfoW;
// ntdll level, so opens from system libraries (WIC, shell) are seen too.
struct UnicodeString { USHORT Length; USHORT MaximumLength; PWSTR Buffer; };
struct ObjectAttributes { ULONG Length; HANDLE RootDirectory; UnicodeString* ObjectName; ULONG Attributes; PVOID SecurityDescriptor; PVOID SecurityQualityOfService; };
using NtCreateFileFn = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, ObjectAttributes*, PVOID, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
using NtOpenFileFn = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, ObjectAttributes*, PVOID, ULONG, ULONG);
NtCreateFileFn Real_NtCreateFile = nullptr;
NtOpenFileFn Real_NtOpenFile = nullptr;
decltype(&DwmSetWindowAttribute) Real_DwmSetWindowAttributeO = DwmSetWindowAttribute;
decltype(&DwmExtendFrameIntoClientArea) Real_DwmExtendFrameO = DwmExtendFrameIntoClientArea;
SRWLOCK g_lock = SRWLOCK_INIT;
bool g_installed = false;
std::set<HWND> g_optedIn;
// Survey log de-duplication.
SRWLOCK g_colorLock = SRWLOCK_INIT;
std::set<std::wstring> g_colorsSeen;

const wchar_t* CallerModule(void* address) {
    static thread_local wchar_t name[MAX_PATH];
    HMODULE module = nullptr;
    name[0] = L'\0';
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &module))
    {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(module, path, MAX_PATH);
        const wchar_t* slash = wcsrchr(path, L'\\');
        wcscpy_s(name, slash ? slash + 1 : path);
    }
    return name;
}

// Office's Mica is a blurred copy of the static wallpaper image. A host backdrop brush is the live blur of whatever
// is behind the window instead (animated wallpapers included), and fits the same slot: both are backdrop brushes.
HRESULT STDMETHODCALLTYPE Hook_BlurredWallpaper(void* self, ICompositionBackdropBrush** result) {
    ComPtr<ICompositor3> compositor;
    HRESULT hr = static_cast<IInspectable*>(self)->QueryInterface(IID_PPV_ARGS(&compositor));
    if (SUCCEEDED(hr) && result) hr = compositor->CreateHostBackdropBrush(result);
    const bool swapped = SUCCEEDED(hr);
    if (!swapped) hr = Real_BlurredWallpaper(self, result);
    Log(L"office: wallpaper brush request %08X -> brush %p (%s) thread %lu from %s", hr, result ? *result : nullptr,
        swapped ? L"host backdrop" : L"Office's own", GetCurrentThreadId(), CallerModule(_ReturnAddress()));
    return hr;
}

// --- Effect graph survey: which effects Office builds, and which brush feeds each named source. ---
std::wstring ClassOf(IInspectable* object) {
    if (!object) return L"null";
    HSTRING name = nullptr;
    if (FAILED(object->GetRuntimeClassName(&name)) || !name) return L"?";
    std::wstring text = WindowsGetStringRawBuffer(name, nullptr);
    WindowsDeleteString(name);
    return text;
}

std::wstring Describe(ABI::Windows::Graphics::Effects::IGraphicsEffectSource* source, int depth) {
    if (!source || depth > 6) return L"-";
    ComPtr<ICompositionEffectSourceParameter> parameter;
    if (SUCCEEDED(source->QueryInterface(IID_PPV_ARGS(&parameter))))
    {
        HSTRING name = nullptr;
        parameter->get_Name(&name);
        std::wstring text = L"param '" + std::wstring(name ? WindowsGetStringRawBuffer(name, nullptr) : L"") + L"'";
        WindowsDeleteString(name);
        return text;
    }
    ComPtr<ABI::Windows::Graphics::Effects::IGraphicsEffectD2D1Interop> effect;
    if (FAILED(source->QueryInterface(IID_PPV_ARGS(&effect)))) return ClassOf(source);
    GUID id{};
    effect->GetEffectId(&id);
    wchar_t guid[64];
    StringFromGUID2(id, guid, 64);
    std::wstring text = std::wstring(guid) + L"(";
    UINT count = 0;
    effect->GetSourceCount(&count);
    for (UINT i = 0; i < count; ++i)
    {
        ComPtr<ABI::Windows::Graphics::Effects::IGraphicsEffectSource> child;
        effect->GetSource(i, &child);
        text += (i ? L", " : L"") + Describe(child.Get(), depth + 1);
    }
    return text + L")";
}

void** g_vtSetSource = nullptr;
void** g_vtPutBrush = nullptr;
using SetSource = HRESULT(STDMETHODCALLTYPE*)(ICompositionEffectBrush*, HSTRING, ICompositionBrush*);
using PutBrush = HRESULT(STDMETHODCALLTYPE*)(ISpriteVisual*, ICompositionBrush*);
SetSource Real_SetSource = nullptr;
PutBrush Real_PutBrush = nullptr;
using CreateFactory = HRESULT(STDMETHODCALLTYPE*)(ICompositor*, ABI::Windows::Graphics::Effects::IGraphicsEffect*, ICompositionEffectFactory**);
using CreateSprite = HRESULT(STDMETHODCALLTYPE*)(ICompositor*, ISpriteVisual**);
CreateFactory Real_CreateFactory = nullptr;
CreateSprite Real_CreateSprite = nullptr;
using FactoryCreateBrush = HRESULT(STDMETHODCALLTYPE*)(ICompositionEffectFactory*, ICompositionEffectBrush**);
FactoryCreateBrush Real_FactoryCreateBrush = nullptr;
void** g_vtFactoryCreate = nullptr;

bool Patch(void** slot, void* value) {
    DWORD protection = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection)) return false;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), protection, &protection);
    return true;
}

HRESULT STDMETHODCALLTYPE Hook_SetSource(ICompositionEffectBrush* self, HSTRING name, ICompositionBrush* brush) {
    Log(L"office: effect brush %p source '%s' = %s %p", self, name ? WindowsGetStringRawBuffer(name, nullptr) : L"", ClassOf(brush).c_str(), brush);
    return Real_SetSource(self, name, brush);
}

HRESULT STDMETHODCALLTYPE Hook_PutBrush(ISpriteVisual* self, ICompositionBrush* brush) {
    // Survey: surface brushes by surface type, size and stretch, once per distinct key.
    ComPtr<ICompositionSurfaceBrush> surfaceBrush;
    if (brush && SUCCEEDED(brush->QueryInterface(IID_PPV_ARGS(&surfaceBrush))))
    {
        ComPtr<ICompositionSurface> surface;
        surfaceBrush->get_Surface(&surface);
        CompositionStretch stretch{};
        surfaceBrush->get_Stretch(&stretch);
        ABI::Windows::Foundation::Size size{};
        ComPtr<ICompositionDrawingSurface> drawing;
        if (surface && SUCCEEDED(surface.As(&drawing))) drawing->get_Size(&size);
        ComPtr<IVisual> visual;
        ABI::Windows::Foundation::Numerics::Vector2 visualSize{};
        if (SUCCEEDED(self->QueryInterface(IID_PPV_ARGS(&visual)))) visual->get_Size(&visualSize);
        wchar_t key[256];
        swprintf_s(key, L"surface brush: %s %.0fx%.0f stretch %d on visual %.0fx%.0f", surface ? ClassOf(surface.Get()).c_str() : L"none",
            size.Width, size.Height, static_cast<int>(stretch), visualSize.X, visualSize.Y);
        AcquireSRWLockExclusive(&g_colorLock);
        const bool fresh = g_colorsSeen.insert(key).second;
        ReleaseSRWLockExclusive(&g_colorLock);
        if (fresh) Log(L"office: %s (brush %p sprite %p)", key, brush, self);
    }
    return Real_PutBrush(self, brush);
}

HRESULT STDMETHODCALLTYPE Hook_FactoryCreateBrush(ICompositionEffectFactory* self, ICompositionEffectBrush** result) {
    const HRESULT hr = Real_FactoryCreateBrush(self, result);
    if (SUCCEEDED(hr) && result && *result)
    {
        Log(L"office: factory %p -> effect brush %p", self, *result);
        if (!g_vtSetSource)
        {
            void** vtable = *reinterpret_cast<void***>(*result);
            Real_SetSource = reinterpret_cast<SetSource>(vtable[7]);
            if (Patch(&vtable[7], reinterpret_cast<void*>(&Hook_SetSource))) g_vtSetSource = &vtable[7];
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateFactory(ICompositor* self, ABI::Windows::Graphics::Effects::IGraphicsEffect* effect, ICompositionEffectFactory** result) {
    const HRESULT hr = Real_CreateFactory(self, effect, result);
    HSTRING name = nullptr;
    if (effect) effect->get_Name(&name);
    ComPtr<ABI::Windows::Graphics::Effects::IGraphicsEffectSource> source;
    if (effect) effect->QueryInterface(IID_PPV_ARGS(&source));
    Log(L"office: effect factory %p '%s' %s from %s", result ? *result : nullptr, name ? WindowsGetStringRawBuffer(name, nullptr) : L"",
        Describe(source.Get(), 0).c_str(), CallerModule(_ReturnAddress()));
    WindowsDeleteString(name);
    if (SUCCEEDED(hr) && result && *result && !g_vtFactoryCreate)
    {
        void** vtable = *reinterpret_cast<void***>(*result);
        Real_FactoryCreateBrush = reinterpret_cast<FactoryCreateBrush>(vtable[6]);
        if (Patch(&vtable[6], reinterpret_cast<void*>(&Hook_FactoryCreateBrush))) g_vtFactoryCreate = &vtable[6];
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateSprite(ICompositor* self, ISpriteVisual** result) {
    const HRESULT hr = Real_CreateSprite(self, result);
    if (SUCCEEDED(hr) && result && *result && !g_vtPutBrush)
    {
        void** vtable = *reinterpret_cast<void***>(*result);
        Real_PutBrush = reinterpret_cast<PutBrush>(vtable[7]);
        if (Patch(&vtable[7], reinterpret_cast<void*>(&Hook_PutBrush))) g_vtPutBrush = &vtable[7];
    }
    return hr;
}

void UndoVtablePatches() {
    if (g_vtSetSource && Patch(g_vtSetSource, reinterpret_cast<void*>(Real_SetSource))) g_vtSetSource = nullptr;
    if (g_vtPutBrush && Patch(g_vtPutBrush, reinterpret_cast<void*>(Real_PutBrush))) g_vtPutBrush = nullptr;
    if (g_vtFactoryCreate && Patch(g_vtFactoryCreate, reinterpret_cast<void*>(Real_FactoryCreateBrush))) g_vtFactoryCreate = nullptr;
}

// --- Gradient survey: Office's title strip is a fixed gradient drawn into a bitmap surface. ---
using RtStops = HRESULT(STDMETHODCALLTYPE*)(void*, const D2D1_GRADIENT_STOP*, UINT32, D2D1_GAMMA, D2D1_EXTEND_MODE, ID2D1GradientStopCollection**);
using DcStops = HRESULT(STDMETHODCALLTYPE*)(void*, const D2D1_GRADIENT_STOP*, UINT32, D2D1_COLOR_SPACE, D2D1_COLOR_SPACE, D2D1_BUFFER_PRECISION, D2D1_EXTEND_MODE, D2D1_COLOR_INTERPOLATION_MODE, ID2D1GradientStopCollection1**);
RtStops Real_RtStops = nullptr;
DcStops Real_DcStops = nullptr;
decltype(&GdiGradientFill) Real_GdiGradientFill = GdiGradientFill;

std::wstring StopsText(const D2D1_GRADIENT_STOP* stops, UINT32 count) {
    std::wstring text;
    for (UINT32 i = 0; i < count && i < 8; ++i)
    {
        wchar_t one[64];
        swprintf_s(one, L" %.2f:#%02X%02X%02X%02X", stops[i].position, static_cast<int>(stops[i].color.a * 255), static_cast<int>(stops[i].color.r * 255),
            static_cast<int>(stops[i].color.g * 255), static_cast<int>(stops[i].color.b * 255));
        text += one;
    }
    return text;
}

HRESULT STDMETHODCALLTYPE Hook_RtStops(void* self, const D2D1_GRADIENT_STOP* stops, UINT32 count, D2D1_GAMMA gamma, D2D1_EXTEND_MODE extend, ID2D1GradientStopCollection** result) {
    Log(L"office: D2D gradient (rt)%s from %s", StopsText(stops, count).c_str(), CallerModule(_ReturnAddress()));
    return Real_RtStops(self, stops, count, gamma, extend, result);
}

HRESULT STDMETHODCALLTYPE Hook_DcStops(void* self, const D2D1_GRADIENT_STOP* stops, UINT32 count, D2D1_COLOR_SPACE pre, D2D1_COLOR_SPACE post, D2D1_BUFFER_PRECISION precision, D2D1_EXTEND_MODE extend, D2D1_COLOR_INTERPOLATION_MODE mode, ID2D1GradientStopCollection1** result) {
    Log(L"office: D2D gradient (dc)%s from %s", StopsText(stops, count).c_str(), CallerModule(_ReturnAddress()));
    return Real_DcStops(self, stops, count, pre, post, precision, extend, mode, result);
}

BOOL WINAPI Hook_GdiGradientFill(HDC dc, PTRIVERTEX vertices, ULONG vertexCount, PVOID mesh, ULONG meshCount, ULONG mode) {
    std::wstring text;
    for (ULONG i = 0; i < vertexCount && i < 4; ++i)
    {
        wchar_t one[64];
        swprintf_s(one, L" (%ld,%ld)#%02X%02X%02X", vertices[i].x, vertices[i].y, vertices[i].Red >> 8, vertices[i].Green >> 8, vertices[i].Blue >> 8);
        text += one;
    }
    Log(L"office: GDI GradientFill mode %lu%s from %s", mode, text.c_str(), CallerModule(_ReturnAddress()));
    return Real_GdiGradientFill(dc, vertices, vertexCount, mesh, meshCount, mode);
}

// Solid colours and clears, logged once per distinct colour and caller.
using RtSolid = HRESULT(STDMETHODCALLTYPE*)(void*, const D2D1_COLOR_F*, const D2D1_BRUSH_PROPERTIES*, ID2D1SolidColorBrush**);
using RtClear = void(STDMETHODCALLTYPE*)(void*, const D2D1_COLOR_F*);
using BrushSetColor = void(STDMETHODCALLTYPE*)(void*, const D2D1_COLOR_F*);
RtSolid Real_DcSolid = nullptr, Real_RtSolid2 = nullptr;
RtClear Real_DcClear = nullptr, Real_RtClear2 = nullptr;
BrushSetColor Real_SetColor = nullptr;
RtStops Real_RtStops2 = nullptr;

void LogColor(const wchar_t* call, const D2D1_COLOR_F* color, void* caller) {
    if (!color) return;
    wchar_t key[160];
    swprintf_s(key, L"%s #%02X%02X%02X%02X from %s", call, static_cast<int>(color->a * 255 + 0.5f), static_cast<int>(color->r * 255 + 0.5f),
        static_cast<int>(color->g * 255 + 0.5f), static_cast<int>(color->b * 255 + 0.5f), CallerModule(caller));
    AcquireSRWLockExclusive(&g_colorLock);
    const bool fresh = g_colorsSeen.insert(key).second;
    ReleaseSRWLockExclusive(&g_colorLock);
    if (fresh) Log(L"office: D2D %s", key);
}

HRESULT STDMETHODCALLTYPE Hook_DcSolid(void* self, const D2D1_COLOR_F* color, const D2D1_BRUSH_PROPERTIES* props, ID2D1SolidColorBrush** result) {
    LogColor(L"solid", color, _ReturnAddress());
    return Real_DcSolid(self, color, props, result);
}
HRESULT STDMETHODCALLTYPE Hook_RtSolid2(void* self, const D2D1_COLOR_F* color, const D2D1_BRUSH_PROPERTIES* props, ID2D1SolidColorBrush** result) {
    LogColor(L"solid(dcrt)", color, _ReturnAddress());
    return Real_RtSolid2(self, color, props, result);
}
void STDMETHODCALLTYPE Hook_DcClear(void* self, const D2D1_COLOR_F* color) {
    LogColor(L"clear", color, _ReturnAddress());
    Real_DcClear(self, color);
}
void STDMETHODCALLTYPE Hook_RtClear2(void* self, const D2D1_COLOR_F* color) {
    LogColor(L"clear(dcrt)", color, _ReturnAddress());
    Real_RtClear2(self, color);
}
void STDMETHODCALLTYPE Hook_SetColor(void* self, const D2D1_COLOR_F* color) {
    LogColor(L"setcolor", color, _ReturnAddress());
    Real_SetColor(self, color);
}
HRESULT STDMETHODCALLTYPE Hook_RtStops2(void* self, const D2D1_GRADIENT_STOP* stops, UINT32 count, D2D1_GAMMA gamma, D2D1_EXTEND_MODE extend, ID2D1GradientStopCollection** result) {
    Log(L"office: D2D gradient (dcrt)%s from %s", StopsText(stops, count).c_str(), CallerModule(_ReturnAddress()));
    return Real_RtStops2(self, stops, count, gamma, extend, result);
}

// Bitmap draws, logged once per destination size and caller: the title strip is a pre-made bitmap.
using RtDrawBitmap = void(STDMETHODCALLTYPE*)(void*, ID2D1Bitmap*, const D2D1_RECT_F*, FLOAT, D2D1_BITMAP_INTERPOLATION_MODE, const D2D1_RECT_F*);
RtDrawBitmap Real_DcDrawBitmap = nullptr, Real_RtDrawBitmap2 = nullptr;
decltype(&BitBlt) Real_BitBltO = BitBlt;
decltype(&StretchBlt) Real_StretchBltO = StretchBlt;
decltype(&GdiAlphaBlend) Real_AlphaBlendO = GdiAlphaBlend;

void LogDraw(const wchar_t* call, float w, float h, float sw, float sh, void* caller, const wchar_t* target) {
    if (w < 300 || h < 20 || h > 400) return; // only wide strips
    wchar_t key[200];
    swprintf_s(key, L"%s dest %.0fx%.0f src %.0fx%.0f into %s from %s", call, w, h, sw, sh, target, CallerModule(caller));
    AcquireSRWLockExclusive(&g_colorLock);
    const bool fresh = g_colorsSeen.insert(key).second;
    ReleaseSRWLockExclusive(&g_colorLock);
    if (fresh) Log(L"office: %s", key);
}

void STDMETHODCALLTYPE Hook_DcDrawBitmap(void* self, ID2D1Bitmap* bitmap, const D2D1_RECT_F* dest, FLOAT opacity, D2D1_BITMAP_INTERPOLATION_MODE mode, const D2D1_RECT_F* src) {
    const D2D1_SIZE_F size = bitmap ? bitmap->GetSize() : D2D1_SIZE_F{};
    if (dest) LogDraw(L"D2D DrawBitmap", dest->right - dest->left, dest->bottom - dest->top, size.width, size.height, _ReturnAddress(), L"d2d");
    Real_DcDrawBitmap(self, bitmap, dest, opacity, mode, src);
}
void STDMETHODCALLTYPE Hook_RtDrawBitmap2(void* self, ID2D1Bitmap* bitmap, const D2D1_RECT_F* dest, FLOAT opacity, D2D1_BITMAP_INTERPOLATION_MODE mode, const D2D1_RECT_F* src) {
    const D2D1_SIZE_F size = bitmap ? bitmap->GetSize() : D2D1_SIZE_F{};
    if (dest) LogDraw(L"D2D(dcrt) DrawBitmap", dest->right - dest->left, dest->bottom - dest->top, size.width, size.height, _ReturnAddress(), L"d2d");
    Real_RtDrawBitmap2(self, bitmap, dest, opacity, mode, src);
}

const wchar_t* DcWindowClass(HDC dc) {
    static thread_local wchar_t name[64];
    name[0] = 0;
    if (HWND window = WindowFromDC(dc)) GetClassNameW(window, name, 64);
    else wcscpy_s(name, L"memdc");
    return name;
}
// Office's title bar and tab row: mso98 renders them, gradient and all, into an offscreen bitmap and blits it into the
// MsoCommandBar in one piece. The gradient only varies left to right, so the top pixel row is pure background. Each
// pixel's difference from its column's background becomes premultiplied alpha: background turns clear (the glass shows
// through the extended frame), text and icons stay as they are.
bool ClearStrip(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy) {
    BITMAPINFO info{};
    info.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
    void* bits = nullptr;
    HDC memory = CreateCompatibleDC(dst);
    HBITMAP bitmap = memory ? CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!bitmap)
    {
        if (memory) DeleteDC(memory);
        return false;
    }
    HGDIOBJ old = SelectObject(memory, bitmap);
    Real_BitBltO(memory, 0, 0, w, h, src, sx, sy, SRCCOPY);
    GdiFlush();
    auto* pixels = static_cast<BYTE*>(bits);
    static LONG s_logged = 0;
    const bool log = InterlockedIncrement(&s_logged) <= 6;
    auto sample = [&](int px, int py) { const BYTE* q = pixels + (static_cast<size_t>(py) * w + px) * 4; return RGB(q[2], q[1], q[0]) | (q[3] << 24); };
    if (log) Log(L"office: strip %dx%d at %d,%d before: (5,0) %08X (300,20) %08X (900,40) %08X", w, h, x, y, sample(5, 0), sample(300, 20), sample(900, 40));
    for (int row = h - 1; row >= 0; --row) // bottom-up, so row 0 (the background reference) is read before it changes
        for (int column = 0; column < w; ++column)
        {
            BYTE* p = pixels + (static_cast<size_t>(row) * w + column) * 4;
            const BYTE* background = pixels + static_cast<size_t>(column) * 4;
            const int b = std::max(0, p[0] - background[0]), g = std::max(0, p[1] - background[1]), r = std::max(0, p[2] - background[2]);
            p[0] = static_cast<BYTE>(b);
            p[1] = static_cast<BYTE>(g);
            p[2] = static_cast<BYTE>(r);
            p[3] = static_cast<BYTE>(std::max({ r, g, b }));
        }
    if (log) Log(L"office: strip after: (5,0) %08X (300,20) %08X (900,40) %08X", sample(5, 0), sample(300, 20), sample(900, 40));
    const BOOL done = Real_BitBltO(dst, x, y, w, h, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    return done != FALSE;
}

BOOL WINAPI Hook_BitBltO(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop) {
    LogDraw(L"BitBlt", static_cast<float>(w), static_cast<float>(h), 0, 0, _ReturnAddress(), DcWindowClass(dst));
    if (rop == SRCCOPY && w >= 300 && h >= 40 && h <= 120 && _wcsicmp(DcWindowClass(dst), L"MsoCommandBar") == 0 &&
        _wcsicmp(CallerModule(_ReturnAddress()), L"mso98win32client.dll") == 0 && ClearStrip(dst, x, y, w, h, src, sx, sy))
        return TRUE;
    return Real_BitBltO(dst, x, y, w, h, src, sx, sy, rop);
}
BOOL WINAPI Hook_StretchBltO(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, DWORD rop) {
    LogDraw(L"StretchBlt", static_cast<float>(w), static_cast<float>(h), static_cast<float>(sw), static_cast<float>(sh), _ReturnAddress(), DcWindowClass(dst));
    return Real_StretchBltO(dst, x, y, w, h, src, sx, sy, sw, sh, rop);
}
BOOL WINAPI Hook_AlphaBlendO(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, BLENDFUNCTION blend) {
    LogDraw(L"AlphaBlend", static_cast<float>(w), static_cast<float>(h), static_cast<float>(sw), static_cast<float>(sh), _ReturnAddress(), DcWindowClass(dst));
    return Real_AlphaBlendO(dst, x, y, w, h, src, sx, sy, sw, sh, blend);
}

// Pixel-data scan: the strip's brown has to come from raw pixels, so look for it in bitmap uploads.
using RtCreateBitmap = HRESULT(STDMETHODCALLTYPE*)(void*, D2D1_SIZE_U, const void*, UINT32, const D2D1_BITMAP_PROPERTIES*, ID2D1Bitmap**);
using DcCreateBitmap = HRESULT(STDMETHODCALLTYPE*)(void*, D2D1_SIZE_U, const void*, UINT32, const D2D1_BITMAP_PROPERTIES1*, ID2D1Bitmap1**);
using CopyFromMemory = HRESULT(STDMETHODCALLTYPE*)(void*, const D2D1_RECT_U*, const void*, UINT32);
RtCreateBitmap Real_RtCreateBitmap = nullptr, Real_RtCreateBitmap2 = nullptr;
DcCreateBitmap Real_DcCreateBitmap = nullptr;
CopyFromMemory Real_CopyFromMemory = nullptr;

void ScanPixels(const wchar_t* call, const void* data, UINT32 pitch, UINT32 width, UINT32 height, void* caller) {
    if (!data || !pitch || !width || !height) return;
    int hits = 0;
    UINT32 hx = 0, hy = 0;
    for (UINT32 y = 0; y < height && y < 512; ++y)
        for (UINT32 x = 0; x < width && x < 4096; ++x)
        {
            const BYTE* p = static_cast<const BYTE*>(data) + static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * 4;
            // Warm near-black: red above blue by 6..14, red between 0x10 and 0x24 (the strip's #1F1B17 .. #120E0A).
            if (p[2] >= 0x10 && p[2] <= 0x24 && p[2] - p[0] >= 6 && p[2] - p[0] <= 14 && p[1] > p[0] && p[1] < p[2])
            {
                if (!hits++) { hx = x; hy = y; }
            }
        }
    wchar_t key[200];
    swprintf_s(key, L"%s %ux%u from %s brown %s", call, width, height, CallerModule(caller), hits ? L"YES" : L"no");
    AcquireSRWLockExclusive(&g_colorLock);
    const bool fresh = g_colorsSeen.insert(key).second;
    ReleaseSRWLockExclusive(&g_colorLock);
    if (fresh || hits) Log(L"office: %s (%d px, first at %u,%u)", key, hits, hx, hy);
}

HRESULT STDMETHODCALLTYPE Hook_RtCreateBitmap(void* self, D2D1_SIZE_U size, const void* data, UINT32 pitch, const D2D1_BITMAP_PROPERTIES* props, ID2D1Bitmap** result) {
    ScanPixels(L"CreateBitmap", data, pitch, size.width, size.height, _ReturnAddress());
    return Real_RtCreateBitmap(self, size, data, pitch, props, result);
}
HRESULT STDMETHODCALLTYPE Hook_RtCreateBitmap2(void* self, D2D1_SIZE_U size, const void* data, UINT32 pitch, const D2D1_BITMAP_PROPERTIES* props, ID2D1Bitmap** result) {
    ScanPixels(L"CreateBitmap(dcrt)", data, pitch, size.width, size.height, _ReturnAddress());
    return Real_RtCreateBitmap2(self, size, data, pitch, props, result);
}
HRESULT STDMETHODCALLTYPE Hook_DcCreateBitmap(void* self, D2D1_SIZE_U size, const void* data, UINT32 pitch, const D2D1_BITMAP_PROPERTIES1* props, ID2D1Bitmap1** result) {
    ScanPixels(L"CreateBitmap1", data, pitch, size.width, size.height, _ReturnAddress());
    return Real_DcCreateBitmap(self, size, data, pitch, props, result);
}
HRESULT STDMETHODCALLTYPE Hook_CopyFromMemory(void* self, const D2D1_RECT_U* rect, const void* data, UINT32 pitch) {
    UINT32 w = 0, h = 0;
    if (rect) { w = rect->right - rect->left; h = rect->bottom - rect->top; }
    else { const D2D1_SIZE_U size = static_cast<ID2D1Bitmap*>(self)->GetPixelSize(); w = size.width; h = size.height; }
    ScanPixels(L"CopyFromMemory", data, pitch, w, h, _ReturnAddress());
    return Real_CopyFromMemory(self, rect, data, pitch);
}

void FindD2DMethods() {
    ComPtr<ID3D11Device> d3d;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
    ComPtr<IDXGIDevice> dxgi;
    if (SUCCEEDED(hr)) hr = d3d.As(&dxgi);
    ComPtr<ID2D1Device> device;
    if (SUCCEEDED(hr)) hr = D2D1CreateDevice(dxgi.Get(), nullptr, &device);
    ComPtr<ID2D1DeviceContext> context;
    if (SUCCEEDED(hr)) hr = device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context);
    if (SUCCEEDED(hr))
    {
        void** vtable = *reinterpret_cast<void***>(context.Get());
        Real_RtStops = reinterpret_cast<RtStops>(vtable[9]);
        Real_DcStops = reinterpret_cast<DcStops>(vtable[64]); // ID2D1RenderTarget's 53 methods end at slot 56; DeviceContext's 8th method
        Real_DcSolid = reinterpret_cast<RtSolid>(vtable[8]);
        Real_DcClear = reinterpret_cast<RtClear>(vtable[47]);
        Real_DcDrawBitmap = reinterpret_cast<RtDrawBitmap>(vtable[26]);
        Real_RtCreateBitmap = reinterpret_cast<RtCreateBitmap>(vtable[4]);
        Real_DcCreateBitmap = reinterpret_cast<DcCreateBitmap>(vtable[57]);
        ComPtr<ID2D1Bitmap> probe;
        if (SUCCEEDED(context->CreateBitmap(D2D1::SizeU(1, 1), nullptr, 0, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &probe)))
            Real_CopyFromMemory = reinterpret_cast<CopyFromMemory>((*reinterpret_cast<void***>(probe.Get()))[10]);
        ComPtr<ID2D1SolidColorBrush> brush;
        const D2D1_COLOR_F black{ 0, 0, 0, 1 };
        if (SUCCEEDED(context->CreateSolidColorBrush(black, &brush))) Real_SetColor = reinterpret_cast<BrushSetColor>((*reinterpret_cast<void***>(brush.Get()))[8]);
    }
    // DC render targets (bound to a window's HDC) may be a different implementation class.
    ComPtr<ID2D1Factory> factory;
    if (SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, factory.GetAddressOf())))
    {
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ComPtr<ID2D1DCRenderTarget> dcrt;
        if (SUCCEEDED(factory->CreateDCRenderTarget(&props, &dcrt)))
        {
            void** vtable = *reinterpret_cast<void***>(dcrt.Get());
            if (vtable[8] != reinterpret_cast<void*>(Real_DcSolid)) Real_RtSolid2 = reinterpret_cast<RtSolid>(vtable[8]);
            if (vtable[47] != reinterpret_cast<void*>(Real_DcClear)) Real_RtClear2 = reinterpret_cast<RtClear>(vtable[47]);
            if (vtable[9] != reinterpret_cast<void*>(Real_RtStops)) Real_RtStops2 = reinterpret_cast<RtStops>(vtable[9]);
            if (vtable[26] != reinterpret_cast<void*>(Real_DcDrawBitmap)) Real_RtDrawBitmap2 = reinterpret_cast<RtDrawBitmap>(vtable[26]);
            if (vtable[4] != reinterpret_cast<void*>(Real_RtCreateBitmap)) Real_RtCreateBitmap2 = reinterpret_cast<RtCreateBitmap>(vtable[4]);
            Log(L"office: dc render target %s", Real_RtSolid2 ? L"is a separate class" : L"shares the device context's methods");
        }
    }
    Log(L"office: d2d methods %08X", hr);
}

HRESULT STDMETHODCALLTYPE Hook_Backdrop(void* self, ICompositionBackdropBrush** result) {
    const HRESULT hr = Real_Backdrop(self, result);
    Log(L"office: CreateBackdropBrush %08X brush %p thread %lu from %s", hr, result ? *result : nullptr, GetCurrentThreadId(), CallerModule(_ReturnAddress()));
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_HostBackdrop(void* self, ICompositionBackdropBrush** result) {
    const HRESULT hr = Real_HostBackdrop(self, result);
    Log(L"office: CreateHostBackdropBrush %08X brush %p thread %lu from %s", hr, result ? *result : nullptr, GetCurrentThreadId(), CallerModule(_ReturnAddress()));
    return hr;
}

bool LooksLikeWallpaper(const wchar_t* path) {
    return path && (StrStrIW(path, L"Wallpaper") || StrStrIW(path, L"\\Themes\\") || StrStrIW(path, L".jpg") || StrStrIW(path, L".jpeg") || StrStrIW(path, L".png"));
}

HANDLE WINAPI Hook_CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE templ) {
    const HANDLE file = Real_CreateFileW(path, access, share, security, disposition, flags, templ);
    if (LooksLikeWallpaper(path)) Log(L"office: CreateFileW %s -> %p thread %lu from %s", path, file, GetCurrentThreadId(), CallerModule(_ReturnAddress()));
    return file;
}

BOOL WINAPI Hook_SystemParametersInfoW(UINT action, UINT param, PVOID data, UINT winIni) {
    const BOOL result = Real_SystemParametersInfoW(action, param, data, winIni);
    if (action == SPI_GETDESKWALLPAPER) Log(L"office: SPI_GETDESKWALLPAPER thread %lu from %s", GetCurrentThreadId(), CallerModule(_ReturnAddress()));
    return result;
}

void LogOpen(const wchar_t* call, ObjectAttributes* attributes, LONG status) {
    if (!attributes || !attributes->ObjectName || !attributes->ObjectName->Buffer) return;
    const std::wstring path(attributes->ObjectName->Buffer, attributes->ObjectName->Length / sizeof(wchar_t));
    if (LooksLikeWallpaper(path.c_str()) || StrStrIW(path.c_str(), L"DSC0")) Log(L"office: %s %s -> %08X thread %lu", call, path.c_str(), status, GetCurrentThreadId());
}

LONG NTAPI Hook_NtCreateFile(PHANDLE file, ACCESS_MASK access, ObjectAttributes* attributes, PVOID io, PLARGE_INTEGER size, ULONG fileAttributes, ULONG share, ULONG disposition, ULONG options, PVOID ea, ULONG eaLength) {
    const LONG status = Real_NtCreateFile(file, access, attributes, io, size, fileAttributes, share, disposition, options, ea, eaLength);
    LogOpen(L"NtCreateFile", attributes, status);
    return status;
}

LONG NTAPI Hook_NtOpenFile(PHANDLE file, ACCESS_MASK access, ObjectAttributes* attributes, PVOID io, ULONG share, ULONG options) {
    const LONG status = Real_NtOpenFile(file, access, attributes, io, share, options);
    LogOpen(L"NtOpenFile", attributes, status);
    return status;
}

HRESULT WINAPI Hook_DwmSetWindowAttributeO(HWND window, DWORD attribute, LPCVOID value, DWORD size) {
    const HRESULT hr = Real_DwmSetWindowAttributeO(window, attribute, value, size);
    Log(L"office: DwmSetWindowAttribute %p attr %lu value %ld size %lu -> %08X from %s", window, attribute,
        value && size >= 4 ? *static_cast<const LONG*>(value) : -1, size, hr, CallerModule(_ReturnAddress()));
    return hr;
}

HRESULT WINAPI Hook_DwmExtendFrameO(HWND window, const MARGINS* margins) {
    const HRESULT hr = Real_DwmExtendFrameO(window, margins);
    if (margins) Log(L"office: DwmExtendFrameIntoClientArea %p %d %d %d %d -> %08X from %s", window, margins->cxLeftWidth,
        margins->cxRightWidth, margins->cyTopHeight, margins->cyBottomHeight, hr, CallerModule(_ReturnAddress()));
    return hr;
}

template <typename I> void* Method(const ComPtr<ICompositor>& compositor, int slot) {
    ComPtr<I> face;
    if (FAILED(compositor.As(&face))) return nullptr;
    return (*reinterpret_cast<void***>(face.Get()))[slot];
}

// A throwaway Compositor on a thread of our own (it needs a DispatcherQueue), only to find the implementation's
// methods. Every Compositor in the process shares them, so detouring the functions catches Office's calls too.
DWORD WINAPI FindMethods(void*) {
    RoInitialize(RO_INIT_SINGLETHREADED);
    {
        DispatcherQueueOptions options{ sizeof options, DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE };
        ComPtr<ABI::Windows::System::IDispatcherQueueController> controller;
        HRESULT hr = CreateDispatcherQueueController(options, &controller);
        ComPtr<IInspectable> instance;
        if (SUCCEEDED(hr)) hr = RoActivateInstance(HStringReference(RuntimeClass_Windows_UI_Composition_Compositor).Get(), &instance);
        ComPtr<ICompositor> compositor;
        if (SUCCEEDED(hr)) hr = instance.As(&compositor);
        if (SUCCEEDED(hr))
        {
            Real_BlurredWallpaper = static_cast<CreateBrush>(Method<ICompositorWithBlurredWallpaperBackdropBrush>(compositor, BlurredWallpaperSlot));
            Real_Backdrop = static_cast<CreateBrush>(Method<ICompositor2>(compositor, BackdropSlot));
            Real_HostBackdrop = static_cast<CreateBrush>(Method<ICompositor3>(compositor, HostBackdropSlot));
            // ICompositor: CreateEffectFactory is slot 11, CreateSpriteVisual slot 22.
            Real_CreateFactory = static_cast<CreateFactory>(Method<ICompositor>(compositor, 11));
            Real_CreateSprite = static_cast<CreateSprite>(Method<ICompositor>(compositor, 22));
        }
        Log(L"office: compositor methods %08X (blurred %p in %s, backdrop %p, host %p)", hr, Real_BlurredWallpaper,
            Real_BlurredWallpaper ? CallerModule(Real_BlurredWallpaper) : L"-", Real_Backdrop, Real_HostBackdrop);
    }
    FindD2DMethods();
    RoUninitialize();
    return 0;
}

template <typename T> void Attach(T& real, T hook) { if (real) DetourAttach(reinterpret_cast<PVOID*>(&real), reinterpret_cast<PVOID>(hook)); }
template <typename T> void Detach(T& real, T hook) { if (real) DetourDetach(reinterpret_cast<PVOID*>(&real), reinterpret_cast<PVOID>(hook)); }
}

void InstallOfficeHooks() {
    AcquireSRWLockExclusive(&g_lock);
    if (!g_installed)
    {
        if (HANDLE thread = CreateThread(nullptr, 0, FindMethods, nullptr, 0, nullptr))
        {
            WaitForSingleObject(thread, 5000);
            CloseHandle(thread);
        }
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        Attach(Real_BlurredWallpaper, &Hook_BlurredWallpaper);
        Attach(Real_Backdrop, &Hook_Backdrop);
        Attach(Real_HostBackdrop, &Hook_HostBackdrop);
        Attach(Real_CreateFileW, &Hook_CreateFileW);
        Real_NtCreateFile = reinterpret_cast<NtCreateFileFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
        Real_NtOpenFile = reinterpret_cast<NtOpenFileFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtOpenFile"));
        Attach(Real_NtCreateFile, &Hook_NtCreateFile);
        Attach(Real_NtOpenFile, &Hook_NtOpenFile);
        Attach(Real_SystemParametersInfoW, &Hook_SystemParametersInfoW);
        Attach(Real_DwmSetWindowAttributeO, &Hook_DwmSetWindowAttributeO);
        Attach(Real_DwmExtendFrameO, &Hook_DwmExtendFrameO);
        Attach(Real_CreateFactory, &Hook_CreateFactory);
        Attach(Real_RtStops, &Hook_RtStops);
        Attach(Real_DcStops, &Hook_DcStops);
        Attach(Real_GdiGradientFill, &Hook_GdiGradientFill);
        Attach(Real_DcSolid, &Hook_DcSolid);
        Attach(Real_DcClear, &Hook_DcClear);
        Attach(Real_SetColor, &Hook_SetColor);
        Attach(Real_RtSolid2, &Hook_RtSolid2);
        Attach(Real_RtClear2, &Hook_RtClear2);
        Attach(Real_RtStops2, &Hook_RtStops2);
        Attach(Real_DcDrawBitmap, &Hook_DcDrawBitmap);
        Attach(Real_RtCreateBitmap, &Hook_RtCreateBitmap);
        Attach(Real_RtCreateBitmap2, &Hook_RtCreateBitmap2);
        Attach(Real_DcCreateBitmap, &Hook_DcCreateBitmap);
        Attach(Real_CopyFromMemory, &Hook_CopyFromMemory);
        Attach(Real_RtDrawBitmap2, &Hook_RtDrawBitmap2);
        Attach(Real_BitBltO, &Hook_BitBltO);
        Attach(Real_StretchBltO, &Hook_StretchBltO);
        Attach(Real_AlphaBlendO, &Hook_AlphaBlendO);
        Attach(Real_CreateSprite, &Hook_CreateSprite);
        const LONG result = DetourTransactionCommit();
        g_installed = result == NO_ERROR;
        Log(L"office: brush detours %s (%ld)", g_installed ? L"installed" : L"FAILED", result);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void RemoveOfficeHooks() {
    AcquireSRWLockExclusive(&g_lock);
    if (g_installed)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        Detach(Real_BlurredWallpaper, &Hook_BlurredWallpaper);
        Detach(Real_Backdrop, &Hook_Backdrop);
        Detach(Real_HostBackdrop, &Hook_HostBackdrop);
        Detach(Real_CreateFileW, &Hook_CreateFileW);
        Detach(Real_NtCreateFile, &Hook_NtCreateFile);
        Detach(Real_NtOpenFile, &Hook_NtOpenFile);
        Detach(Real_SystemParametersInfoW, &Hook_SystemParametersInfoW);
        Detach(Real_DwmSetWindowAttributeO, &Hook_DwmSetWindowAttributeO);
        Detach(Real_DwmExtendFrameO, &Hook_DwmExtendFrameO);
        Detach(Real_CreateFactory, &Hook_CreateFactory);
        Detach(Real_RtStops, &Hook_RtStops);
        Detach(Real_DcStops, &Hook_DcStops);
        Detach(Real_GdiGradientFill, &Hook_GdiGradientFill);
        Detach(Real_DcSolid, &Hook_DcSolid);
        Detach(Real_DcClear, &Hook_DcClear);
        Detach(Real_SetColor, &Hook_SetColor);
        Detach(Real_RtSolid2, &Hook_RtSolid2);
        Detach(Real_RtClear2, &Hook_RtClear2);
        Detach(Real_RtStops2, &Hook_RtStops2);
        Detach(Real_DcDrawBitmap, &Hook_DcDrawBitmap);
        Detach(Real_RtCreateBitmap, &Hook_RtCreateBitmap);
        Detach(Real_RtCreateBitmap2, &Hook_RtCreateBitmap2);
        Detach(Real_DcCreateBitmap, &Hook_DcCreateBitmap);
        Detach(Real_CopyFromMemory, &Hook_CopyFromMemory);
        Detach(Real_RtDrawBitmap2, &Hook_RtDrawBitmap2);
        Detach(Real_BitBltO, &Hook_BitBltO);
        Detach(Real_StretchBltO, &Hook_StretchBltO);
        Detach(Real_AlphaBlendO, &Hook_AlphaBlendO);
        Detach(Real_CreateSprite, &Hook_CreateSprite);
        UndoVtablePatches();
        g_installed = DetourTransactionCommit() != NO_ERROR;
        Log(L"office: brush detours removed");
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void OptInOfficeWindow(HWND window) {
    AcquireSRWLockExclusive(&g_lock);
    const bool added = g_optedIn.insert(window).second;
    ReleaseSRWLockExclusive(&g_lock);
    if (!added) return;
    // Host backdrop brushes only render inside windows that opt in.
    const BOOL on = TRUE;
    Log(L"office: host backdrop enabled on %p %08X", window, DwmSetWindowAttribute(window, DWMWA_USE_HOSTBACKDROPBRUSH, &on, sizeof on));
    // The main window also gets Acrylic behind its whole client area, for the cleared title strip to show.
    wchar_t name[32]{};
    GetClassNameW(window, name, _countof(name));
    if (_wcsicmp(name, L"OpusApp") != 0) return;
    const int acrylic = DWMSBT_TRANSIENTWINDOW;
    const MARGINS whole{ -1, -1, -1, -1 };
    DwmSetWindowAttribute(window, DWMWA_SYSTEMBACKDROP_TYPE, &acrylic, sizeof acrylic);
    DwmExtendFrameIntoClientArea(window, &whole);
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
    Log(L"office: acrylic and whole-window frame on %p", window);
}
