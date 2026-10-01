// UWP apps (Settings) get their Mica from inside their own process: WinUI 2's BackdropMaterial hands a wallpaper
// brush to the XAML Window's system backdrop slot (ICompositionSupportsSystemBackdrop), which ApplicationFrameHost
// composites behind the content. Emptying the slot only leaves black: the frame host draws nothing there and the
// DWM backdrop doesn't reach through. So the slot gets a host backdrop brush instead: the live, blurred view of
// whatever is behind the window (animated wallpapers included), the same source DWM's Acrylic is built from.
#include <windows.h>
#include <dwmapi.h>
#include <utility>
#include <roapi.h>
#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>
#undef GetCurrentTime // windows.h macro that clashes with a WinRT method name
#include <windows.ui.xaml.h>
#include <windows.ui.composition.h>
#include "uwp.h"
#include "log.h"

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;
using namespace ABI::Windows::UI::Xaml;
using namespace ABI::Windows::UI::Composition;

namespace {
// Per UI thread: the XAML Window's backdrop slot, the brush the app last asked for (given back on restore) and ours.
thread_local ComPtr<ICompositionSupportsSystemBackdrop> t_slot;
thread_local ComPtr<ICompositionBrush> t_original;
thread_local ComPtr<ICompositionBrush> t_glass;
thread_local bool t_reported;

// The app re-sets its backdrop on its own (BackdropMaterial swaps Mica for a flat fallback while the window is
// inactive). Its put_SystemBackdrop is redirected so every request is remembered and ours stays in place: nothing
// to race, nothing to flash. Slot 7 of the interface: IUnknown (3), IInspectable (3), get_SystemBackdrop.
using PutSystemBackdrop = HRESULT(STDMETHODCALLTYPE*)(ICompositionSupportsSystemBackdrop*, ICompositionBrush*);
constexpr int PutSystemBackdropSlot = 7;
void** g_patchedSlot = nullptr;
PutSystemBackdrop g_realPut = nullptr;

HRESULT STDMETHODCALLTYPE HookedPut(ICompositionSupportsSystemBackdrop* self, ICompositionBrush* brush) {
    if (!t_glass || brush == t_glass.Get()) return g_realPut(self, brush);
    t_original = brush;
    return g_realPut(self, t_glass.Get());
}

bool PatchSlot(void** slot, void* value) {
    DWORD protection = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection)) return false;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return true;
}

void RedirectPut(ICompositionSupportsSystemBackdrop* slot) {
    if (g_patchedSlot) return;
    auto** vtable = *reinterpret_cast<void***>(slot);
    g_realPut = reinterpret_cast<PutSystemBackdrop>(vtable[PutSystemBackdropSlot]);
    if (PatchSlot(&vtable[PutSystemBackdropSlot], reinterpret_cast<void*>(&HookedPut))) g_patchedSlot = &vtable[PutSystemBackdropSlot];
    Log(L"UWP backdrop calls %s", g_patchedSlot ? L"redirected" : L"NOT redirected");
}

void UndoRedirect() {
    if (g_patchedSlot && PatchSlot(g_patchedSlot, reinterpret_cast<void*>(g_realPut))) g_patchedSlot = nullptr;
}

ComPtr<ICompositionSupportsSystemBackdrop> SlotForCurrentThread() {
    ComPtr<IWindowStatics> statics;
    HRESULT result = RoGetActivationFactory(HStringReference(RuntimeClass_Windows_UI_Xaml_Window).Get(), IID_PPV_ARGS(&statics));
    ComPtr<IWindow> window;
    if (SUCCEEDED(result)) result = statics->get_Current(&window);
    ComPtr<ICompositionSupportsSystemBackdrop> slot;
    if (SUCCEEDED(result) && window) result = window.As(&slot);
    if (!slot && !std::exchange(t_reported, true))
        Log(L"UWP backdrop slot unavailable %08X (factory %s, window %s)", result, statics ? L"ok" : L"none", window ? L"ok" : L"none");
    return slot;
}

ComPtr<ICompositionBrush> CreateGlassBrush() {
    ComPtr<IWindowStatics> statics;
    ComPtr<IWindow> window;
    ComPtr<IWindow3> window3;
    ComPtr<ICompositor> compositor;
    ComPtr<ICompositor3> compositor3;
    ComPtr<ICompositionBackdropBrush> backdrop;
    ComPtr<ICompositionBrush> brush;
    HRESULT result = RoGetActivationFactory(HStringReference(RuntimeClass_Windows_UI_Xaml_Window).Get(), IID_PPV_ARGS(&statics));
    if (SUCCEEDED(result)) result = statics->get_Current(&window);
    if (SUCCEEDED(result)) result = window.As(&window3);
    if (SUCCEEDED(result)) result = window3->get_Compositor(&compositor);
    if (SUCCEEDED(result)) result = compositor.As(&compositor3);
    if (SUCCEEDED(result)) result = compositor3->CreateHostBackdropBrush(&backdrop);
    if (SUCCEEDED(result)) result = backdrop.As(&brush);
    Log(L"UWP host backdrop brush %08X", result);
    return brush;
}
}

void ApplyUwpGlass(HWND frame) {
    if (!t_slot) t_slot = SlotForCurrentThread();
    if (!t_slot) return;
    if (!t_glass)
    {
        // The host backdrop brush only renders for windows that opt in.
        const BOOL on = TRUE;
        DwmSetWindowAttribute(frame, DWMWA_USE_HOSTBACKDROPBRUSH, &on, sizeof on);
        t_glass = CreateGlassBrush();
        if (!t_glass) return;
        RedirectPut(t_slot.Get());
    }
    ComPtr<ICompositionBrush> current;
    if (FAILED(t_slot->get_SystemBackdrop(&current)) || current.Get() == t_glass.Get()) return;
    if (current) t_original = current;
    Log(L"UWP backdrop replaced %08X", g_realPut ? g_realPut(t_slot.Get(), t_glass.Get()) : t_slot->put_SystemBackdrop(t_glass.Get()));
}

void UndoUwpRedirect() { UndoRedirect(); }

void RestoreUwpBackdrop() {
    // Undo the redirect first, so the app's own brush goes back through the real method.
    UndoRedirect();
    if (t_slot && t_original) Log(L"UWP backdrop restored %08X", t_slot->put_SystemBackdrop(t_original.Get()));
    t_original.Reset();
    t_glass.Reset();
    t_slot.Reset();
}
