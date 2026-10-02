// Microsoft Office (Word, Excel, PowerPoint): Office draws its own Mica instead of asking DWM for one. Its UI library
// (mso40uiwin32client.dll) builds effect graphs (a wallpaper brush blended with tint and luminosity floods) on
// Compositor::TryCreateBlurredWallpaperBackdropBrush, a blur of the static wallpaper image, so animated wallpapers never
// show. That method is detoured: every request makes both Office's brush and a host backdrop brush (the live blur of
// whatever is behind the window, the same source DWM's Acrylic uses), and hands out the one for the current state.
// Office's effect inputs are meant to be recorded through ICompositionEffectBrush::SetSourceParameter, so turning the
// glass off (or on again) could plug the other brush into every input without restarting Office. UNVERIFIED: in the
// first live test no input was recorded ("0 of 0 inputs switched"), so for now Office needs a restart either way.
//
// Office makes its first brushes (the title bar and tab row among them) while the app starts, before the hooked UI thread
// runs any hook code, so DllMain starts the detour on a thread of its own (StartOfficeHooksEarly).
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <roapi.h>
#include <winstring.h>
#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>
#include <DispatcherQueue.h>
#include <windows.ui.composition.h>
#include <windows.system.h>
#include <intrin.h>
#include <set>
#include <string>
#include <vector>
#include <memory>
#include <utility>
#include "detours.h"
#include "log.h"
#include "office.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;
using namespace ABI::Windows::UI::Composition;
using ABI::Windows::Graphics::Effects::IGraphicsEffect;

namespace {
using CreateBrush = HRESULT(STDMETHODCALLTYPE*)(void* self, ICompositionBackdropBrush** result);
using CreateFactory = HRESULT(STDMETHODCALLTYPE*)(void* self, IGraphicsEffect* effect, ICompositionEffectFactory** result);
using CreateFactoryWithProperties = HRESULT(STDMETHODCALLTYPE*)(void* self, IGraphicsEffect* effect,
    ABI::Windows::Foundation::Collections::IIterable<HSTRING>* properties, ICompositionEffectFactory** result);
using FactoryCreateBrush = HRESULT(STDMETHODCALLTYPE*)(ICompositionEffectFactory* self, ICompositionEffectBrush** result);
using SetSource = HRESULT(STDMETHODCALLTYPE*)(ICompositionEffectBrush* self, HSTRING name, ICompositionBrush* brush);
// Vtable slots: IUnknown (3) + IInspectable (3), then each interface's methods in declaration order.
constexpr int BlurredWallpaperSlot = 6; // ICompositorWithBlurredWallpaperBackdropBrush::TryCreateBlurredWallpaperBackdropBrush
constexpr int CreateFactorySlot = 11;   // ICompositor::CreateEffectFactory
constexpr int CreateFactoryWithPropertiesSlot = 12;
constexpr int FactoryCreateBrushSlot = 6; // ICompositionEffectFactory::CreateBrush
constexpr int SetSourceSlot = 7;          // ICompositionEffectBrush::SetSourceParameter
constexpr UINT_PTR SubclassId = 0x4F464643; // 'OFFC'

CreateBrush Real_BlurredWallpaper = nullptr;
CreateFactory Real_CreateFactory = nullptr;
CreateFactoryWithProperties Real_CreateFactoryWithProperties = nullptr;
FactoryCreateBrush Real_FactoryCreateBrush = nullptr;
SetSource Real_SetSource = nullptr;
void** g_factorySlot = nullptr; // patched vtable entries (shared by every instance of the class)
void** g_setSourceSlot = nullptr;

SRWLOCK g_lock = SRWLOCK_INIT;
bool g_installed = false;
bool g_glassOn = false;
std::set<HWND> g_windows; // main windows opted in and subclassed

// One wallpaper brush request: Office's own brush and the live one, compared as ICompositionBrush identities.
struct BrushPair { ComPtr<ICompositionBrush> original, glass; };
std::vector<BrushPair> g_pairs;
// Every effect input Office plugged one of them into.
struct Input { ComPtr<ICompositionEffectBrush> effect; std::wstring name; size_t pair; };
std::vector<Input> g_inputs;

const wchar_t* CallerModule(void* address) {
    static thread_local wchar_t name[MAX_PATH];
    HMODULE module = nullptr;
    name[0] = 0;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &module))
    {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(module, path, MAX_PATH);
        const wchar_t* slash = wcsrchr(path, L'\\');
        wcscpy_s(name, slash ? slash + 1 : path);
    }
    return name;
}

bool PatchSlot(void** slot, void* value) {
    DWORD protection = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection)) return false;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), protection, &protection);
    return true;
}

ComPtr<ICompositionBrush> AsBrush(IUnknown* object) {
    ComPtr<ICompositionBrush> brush;
    if (object) object->QueryInterface(IID_PPV_ARGS(&brush));
    return brush;
}

HRESULT STDMETHODCALLTYPE Hook_BlurredWallpaper(void* self, ICompositionBackdropBrush** result) {
    if (!result) return Real_BlurredWallpaper(self, result);
    ComPtr<ICompositionBackdropBrush> original, glass;
    const HRESULT hr = Real_BlurredWallpaper(self, &original);
    ComPtr<ICompositor3> compositor;
    if (SUCCEEDED(static_cast<IInspectable*>(self)->QueryInterface(IID_PPV_ARGS(&compositor)))) compositor->CreateHostBackdropBrush(&glass);
    AcquireSRWLockExclusive(&g_lock);
    const bool on = g_glassOn;
    if (original && glass) g_pairs.push_back({ AsBrush(original.Get()), AsBrush(glass.Get()) });
    ReleaseSRWLockExclusive(&g_lock);
    const bool swapped = on && glass;
    *result = (swapped ? glass : original).Detach();
    Log(L"office: wallpaper brush request %08X -> %s, from %s", hr, swapped ? L"host backdrop" : L"Office's own", CallerModule(_ReturnAddress()));
    return swapped ? S_OK : hr;
}

// Records Office plugging one of our pair brushes into an effect input, and keeps it the brush for the current state.
HRESULT STDMETHODCALLTYPE Hook_SetSource(ICompositionEffectBrush* self, HSTRING name, ICompositionBrush* brush) {
    ComPtr<ICompositionBrush> given = AsBrush(brush);
    ICompositionBrush* use = brush;
    AcquireSRWLockExclusive(&g_lock);
    for (size_t i = 0; given && i < g_pairs.size(); ++i)
    {
        if (given.Get() != g_pairs[i].original.Get() && given.Get() != g_pairs[i].glass.Get()) continue;
        const std::wstring input = name ? WindowsGetStringRawBuffer(name, nullptr) : L"";
        bool known = false;
        for (const auto& entry : g_inputs) known |= entry.effect.Get() == self && entry.name == input;
        if (!known) g_inputs.push_back({ self, input, i });
        use = (g_glassOn ? g_pairs[i].glass : g_pairs[i].original).Get();
        break;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return Real_SetSource(self, name, use);
}

// SetSourceParameter is only reachable through an effect brush, so the first factory's CreateBrush leads to it.
HRESULT STDMETHODCALLTYPE Hook_FactoryCreateBrush(ICompositionEffectFactory* self, ICompositionEffectBrush** result) {
    const HRESULT hr = Real_FactoryCreateBrush(self, result);
    if (SUCCEEDED(hr) && result && *result && !g_setSourceSlot)
    {
        void** vtable = *reinterpret_cast<void***>(*result);
        Real_SetSource = reinterpret_cast<SetSource>(vtable[SetSourceSlot]);
        if (PatchSlot(&vtable[SetSourceSlot], reinterpret_cast<void*>(&Hook_SetSource))) g_setSourceSlot = &vtable[SetSourceSlot];
    }
    return hr;
}

void PatchFactory(ICompositionEffectFactory* factory) {
    if (!factory || g_factorySlot) return;
    void** vtable = *reinterpret_cast<void***>(factory);
    Real_FactoryCreateBrush = reinterpret_cast<FactoryCreateBrush>(vtable[FactoryCreateBrushSlot]);
    if (PatchSlot(&vtable[FactoryCreateBrushSlot], reinterpret_cast<void*>(&Hook_FactoryCreateBrush))) g_factorySlot = &vtable[FactoryCreateBrushSlot];
}

HRESULT STDMETHODCALLTYPE Hook_CreateFactory(void* self, IGraphicsEffect* effect, ICompositionEffectFactory** result) {
    const HRESULT hr = Real_CreateFactory(self, effect, result);
    if (SUCCEEDED(hr) && result) PatchFactory(*result);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateFactoryWithProperties(void* self, IGraphicsEffect* effect,
    ABI::Windows::Foundation::Collections::IIterable<HSTRING>* properties, ICompositionEffectFactory** result) {
    const HRESULT hr = Real_CreateFactoryWithProperties(self, effect, properties, result);
    if (SUCCEEDED(hr) && result) PatchFactory(*result);
    return hr;
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
        ComPtr<ICompositorWithBlurredWallpaperBackdropBrush> wallpaper;
        ComPtr<ICompositor> compositor;
        if (SUCCEEDED(hr)) hr = instance.As(&wallpaper);
        if (SUCCEEDED(hr)) hr = instance.As(&compositor);
        if (SUCCEEDED(hr))
        {
            Real_BlurredWallpaper = static_cast<CreateBrush>((*reinterpret_cast<void***>(wallpaper.Get()))[BlurredWallpaperSlot]);
            void** vtable = *reinterpret_cast<void***>(compositor.Get());
            Real_CreateFactory = static_cast<CreateFactory>(vtable[CreateFactorySlot]);
            Real_CreateFactoryWithProperties = static_cast<CreateFactoryWithProperties>(vtable[CreateFactoryWithPropertiesSlot]);
        }
        Log(L"office: compositor methods %08X (%p in %s)", hr, Real_BlurredWallpaper, Real_BlurredWallpaper ? CallerModule(Real_BlurredWallpaper) : L"-");
    }
    RoUninitialize();
    return 0;
}

// Composition objects belong to their compositor's thread: each input is switched there, through its dispatcher queue
// (directly when this already is that thread, as waiting on our own queue would deadlock).
void ApplyInputs(bool on) {
    AcquireSRWLockShared(&g_lock);
    std::vector<std::pair<Input, ComPtr<ICompositionBrush>>> work;
    for (const auto& input : g_inputs) work.push_back({ input, on ? g_pairs[input.pair].glass : g_pairs[input.pair].original });
    ReleaseSRWLockShared(&g_lock);
    int switched = 0;
    for (auto& [input, brush] : work)
    {
        auto apply = [effect = input.effect, name = input.name, brush]() {
            return Real_SetSource(effect.Get(), HStringReference(name.c_str()).Get(), brush.Get());
        };
        ComPtr<ICompositionObject3> object;
        ComPtr<ABI::Windows::System::IDispatcherQueue> queue;
        ComPtr<ABI::Windows::System::IDispatcherQueue2> queue2;
        boolean here = false;
        if (FAILED(input.effect.As(&object)) || FAILED(object->get_DispatcherQueue(&queue)) || !queue) continue;
        if (SUCCEEDED(queue.As(&queue2))) queue2->get_HasThreadAccess(&here);
        if (here)
        {
            switched += SUCCEEDED(apply());
            continue;
        }
        // Shared with the queued call, which may still run after a timed-out wait (never hang the app's UI thread on it).
        struct Pending { HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr); HRESULT result = E_FAIL; ~Pending() { CloseHandle(done); } };
        auto pending = std::make_shared<Pending>();
        boolean queued = false;
        queue->TryEnqueue(Callback<ABI::Windows::System::IDispatcherQueueHandler>([pending, apply]() {
            pending->result = apply();
            SetEvent(pending->done);
            return S_OK;
        }).Get(), &queued);
        if (queued && WaitForSingleObject(pending->done, 2000) == WAIT_OBJECT_0) switched += SUCCEEDED(pending->result);
    }
    Log(L"office: %d of %zu inputs switched to %s", switched, work.size(), on ? L"glass" : L"Office's own");
}

// DWM stops drawing host backdrop brushes in an inactive window, and Office flattens its Mica when deactivated: both
// would turn the glass black on focus out. The window keeps believing it is active (as for Explorer).
//
// KNOWN ISSUE (parked): because the window believes it is active while another window is really foreground, the first
// click on an Office window can be spent on activation, so the title bar may take a second click before it drags. This
// is the price of keeping the glass when unfocused; without the subclass the glass goes flat/black instead. Revisit if
// both can be had at once (e.g. answering WM_MOUSEACTIVATE, or holding the active state only while visually unfocused).
LRESULT CALLBACK KeepActive(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (message == WM_NCACTIVATE) return DefSubclassProc(window, message, TRUE, lParam);
    if (message == WM_ACTIVATEAPP && !wParam) return 0;
    if (message == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE) return 0;
    if (message == WM_NCDESTROY)
    {
        RemoveWindowSubclass(window, KeepActive, SubclassId);
        AcquireSRWLockExclusive(&g_lock);
        g_windows.erase(window);
        ReleaseSRWLockExclusive(&g_lock);
    }
    return DefSubclassProc(window, message, wParam, lParam);
}
}

void InstallOfficeHooks() {
    AcquireSRWLockExclusive(&g_lock);
    const bool wasOn = g_glassOn;
    g_glassOn = true;
    if (!g_installed)
    {
        if (!Real_BlurredWallpaper)
            if (HANDLE thread = CreateThread(nullptr, 0, FindMethods, nullptr, 0, nullptr))
            {
                WaitForSingleObject(thread, 5000);
                CloseHandle(thread);
            }
        if (Real_BlurredWallpaper)
        {
            // Once Office holds our brushes and the patched vtables, the DLL must stay until the app exits.
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&Hook_BlurredWallpaper), &self);
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(reinterpret_cast<PVOID*>(&Real_BlurredWallpaper), reinterpret_cast<PVOID>(&Hook_BlurredWallpaper));
            if (Real_CreateFactory) DetourAttach(reinterpret_cast<PVOID*>(&Real_CreateFactory), reinterpret_cast<PVOID>(&Hook_CreateFactory));
            if (Real_CreateFactoryWithProperties) DetourAttach(reinterpret_cast<PVOID*>(&Real_CreateFactoryWithProperties), reinterpret_cast<PVOID>(&Hook_CreateFactoryWithProperties));
            const LONG result = DetourTransactionCommit();
            g_installed = result == NO_ERROR;
            Log(L"office: brush detours %s (%ld)", g_installed ? L"installed" : L"FAILED", result);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!wasOn) ApplyInputs(true);
}

// Called from DllMain: the DLL lands in the Office process well before the hooked UI thread runs any hook code, and Office makes its
// wallpaper brushes in that gap. A thread of our own installs the detour at once (it starts after the loader lock).
void StartOfficeHooksEarly() {
    if (HANDLE thread = CreateThread(nullptr, 0, [](void*) -> DWORD { InstallOfficeHooks(); return 0; }, nullptr, 0, nullptr))
        CloseHandle(thread);
}

// Turning the glass off: the detours stay (the DLL is pinned) and only hand out Office's own brushes from now on.
void DisableOfficeGlass() {
    AcquireSRWLockExclusive(&g_lock);
    const bool wasOn = std::exchange(g_glassOn, false);
    ReleaseSRWLockExclusive(&g_lock);
    if (wasOn) ApplyInputs(false);
}

// Process exit only (DllMain): the DLL is pinned once installed, so this never runs under a live Office.
void RemoveOfficeHooks() {
    AcquireSRWLockExclusive(&g_lock);
    if (g_installed)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(reinterpret_cast<PVOID*>(&Real_BlurredWallpaper), reinterpret_cast<PVOID>(&Hook_BlurredWallpaper));
        if (Real_CreateFactory) DetourDetach(reinterpret_cast<PVOID*>(&Real_CreateFactory), reinterpret_cast<PVOID>(&Hook_CreateFactory));
        if (Real_CreateFactoryWithProperties) DetourDetach(reinterpret_cast<PVOID*>(&Real_CreateFactoryWithProperties), reinterpret_cast<PVOID>(&Hook_CreateFactoryWithProperties));
        g_installed = DetourTransactionCommit() != NO_ERROR;
    }
    if (g_setSourceSlot && PatchSlot(g_setSourceSlot, reinterpret_cast<void*>(Real_SetSource))) g_setSourceSlot = nullptr;
    if (g_factorySlot && PatchSlot(g_factorySlot, reinterpret_cast<void*>(Real_FactoryCreateBrush))) g_factorySlot = nullptr;
    ReleaseSRWLockExclusive(&g_lock);
}

// Runs on the window's own thread (from the message hook).
void OptInOfficeWindow(HWND window) {
    wchar_t name[32]{};
    GetClassNameW(window, name, _countof(name));
    // Word, Excel and PowerPoint main windows, plus Teams' TeamsWebView (its chrome uses host backdrop brushes; the
    // WebView2 chat pane stays opaque). All office.cpp does for Teams is this opt-in and keep-active.
    const bool mainWindow = _wcsicmp(name, L"OpusApp") == 0 || _wcsicmp(name, L"XLMAIN") == 0 || _wcsicmp(name, L"PPTFrameClass") == 0
        || _wcsicmp(name, L"TeamsWebView") == 0;
    if (!mainWindow) return;
    AcquireSRWLockExclusive(&g_lock);
    const bool added = g_windows.insert(window).second;
    ReleaseSRWLockExclusive(&g_lock);
    if (!added) return;
    // Host backdrop brushes only render inside windows that opt in.
    const BOOL on = TRUE;
    const HRESULT hr = DwmSetWindowAttribute(window, DWMWA_USE_HOSTBACKDROPBRUSH, &on, sizeof on);
    // A subclass must never outlive the DLL; InstallOfficeHooks pins it, this covers a window opted in without it.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&KeepActive), &self);
    SetWindowSubclass(window, KeepActive, SubclassId, 0);
    if (GetForegroundWindow() != window) SendMessageW(window, WM_NCACTIVATE, TRUE, 0);
    Log(L"office: window %p opted in (host backdrop %08X, kept active)", window, hr);
}

// Also on the window's thread (the Detach message): drops keep-active and lets the window look inactive again.
void ReleaseOfficeWindows() {
    AcquireSRWLockExclusive(&g_lock);
    const std::set<HWND> windows = g_windows;
    ReleaseSRWLockExclusive(&g_lock);
    for (HWND window : windows)
    {
        if (GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId()) continue;
        RemoveWindowSubclass(window, KeepActive, SubclassId);
        if (GetForegroundWindow() != window)
        {
            SendMessageW(window, WM_NCACTIVATE, FALSE, 0);
            SendMessageW(window, WM_ACTIVATE, WA_INACTIVE, 0);
        }
        AcquireSRWLockExclusive(&g_lock);
        g_windows.erase(window);
        ReleaseSRWLockExclusive(&g_lock);
        Log(L"office: window %p released", window);
    }
}
