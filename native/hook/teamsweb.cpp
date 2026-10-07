// Teams with "Glass inside the app": instead of keying colors out of the finished frame (which can't tell a dark photo
// pixel from a dark panel), the page itself leaves its big panels clear. The hook is in ms-teams.exe from launch, so it
// catches CreateCoreWebView2EnvironmentWithOptions (WebView2Loader.dll) and every web view Teams then creates:
// - the controller's default background becomes transparent, so Chromium draws real alpha where nothing is painted;
// - a script, run in every document, clears the background of large, square, dark, neutral panels and bars only
//   (the app rail, the title and header bars, the chat list, the conversation). Text, borders, message bubbles, images and video are never touched.
// The DXGI hook in the GPU process then passes that alpha through (GLASSFORGE_DXGI_TUNING gain 0, dxgiglass.cpp).
#include <windows.h>
#include <wrl.h>
#include "WebView2.h"
#include "detours.h"
#include "log.h"
#include "teamsweb.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace
{
    constexpr wchar_t PanelScript[] = LR"JS((() => {
  if (window.__glassForgePanels) return;
  window.__glassForgePanels = true;
  const media = new Set(['IMG', 'VIDEO', 'CANVAS', 'PICTURE', 'SVG', 'IFRAME']);
  const clear = () => {
    const root = document.documentElement;
    if (!root) return;
    root.style.setProperty('background', 'transparent', 'important');
    if (document.body) document.body.style.setProperty('background', 'transparent', 'important');
    const vw = innerWidth, vh = innerHeight;
    for (const el of document.querySelectorAll('body *')) {
      if (media.has(el.tagName.toUpperCase())) continue;
      const box = el.getBoundingClientRect();
      // Panels and bars only: a quarter of the width or a third of the height, and not tiny.
      if ((box.width < vw * 0.25 && box.height < vh * 0.35) || box.width * box.height < vw * vh * 0.01) continue;
      const style = getComputedStyle(el);
      // Message bubbles, buttons, cards and the search box have rounded corners; panels and bars are square.
      if (parseFloat(style.borderTopLeftRadius) > 2 || parseFloat(style.borderBottomRightRadius) > 2) continue;
      if (style.backgroundImage !== 'none') continue;
      const m = style.backgroundColor.match(/rgba?\(([^)]+)\)/);
      if (!m) continue;
      const [r, g, b, a = 1] = m[1].split(',').map(Number);
      if (a === 0) continue;
      // Dark, colorless surfaces (the theme's backgrounds), not accent or content colors.
      if (Math.max(r, g, b) > 64 || Math.max(r, g, b) - Math.min(r, g, b) > 10) continue;
      el.style.setProperty('background-color', 'transparent', 'important');
    }
  };
  let pending = 0;
  const later = () => { if (!pending) pending = setTimeout(() => { pending = 0; clear(); }, 400); };
  new MutationObserver(later).observe(document, { childList: true, subtree: true, attributes: true, attributeFilter: ['class', 'style'] });
  addEventListener('resize', later);
  document.addEventListener('DOMContentLoaded', clear);
  setInterval(clear, 3000);
  clear();
})();)JS";

    using CreateEnvironmentFn = HRESULT(STDAPICALLTYPE*)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
    CreateEnvironmentFn Real_CreateEnvironment = nullptr;

    // ICoreWebView2Environment10's vtable (one inheritance chain, so one table covers every version):
    // IUnknown (3), then CreateCoreWebView2Controller = 3, CreateCoreWebView2CompositionController = 9,
    // CreateCoreWebView2ControllerWithOptions = 21, CreateCoreWebView2CompositionControllerWithOptions = 22.
    using CreateControllerFn = HRESULT(STDMETHODCALLTYPE*)(void*, HWND, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*);
    using CreateCompositionFn = HRESULT(STDMETHODCALLTYPE*)(void*, HWND, ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler*);
    using CreateControllerWithOptionsFn = HRESULT(STDMETHODCALLTYPE*)(void*, HWND, void*, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*);
    using CreateCompositionWithOptionsFn = HRESULT(STDMETHODCALLTYPE*)(void*, HWND, void*, ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler*);
    CreateControllerFn Real_CreateController = nullptr;
    CreateControllerFn Real_CreateControllerBase = nullptr; // when the base interface has a table of its own
    CreateCompositionFn Real_CreateComposition = nullptr;
    CreateControllerWithOptionsFn Real_CreateControllerWithOptions = nullptr;
    CreateCompositionWithOptionsFn Real_CreateCompositionWithOptions = nullptr;
    bool g_environmentPatched = false;

    bool PatchSlot(void** slot, void* value)
    {
        DWORD protection = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection)) return false;
        *slot = value;
        VirtualProtect(slot, sizeof(void*), protection, &protection);
        return true;
    }

    void Prepare(ICoreWebView2Controller* controller)
    {
        if (!controller) return;
        ComPtr<ICoreWebView2Controller2> controller2;
        HRESULT background = E_NOINTERFACE;
        if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&controller2))))
            background = controller2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{ 0, 0, 0, 0 });
        ComPtr<ICoreWebView2> webView;
        HRESULT script = E_FAIL;
        if (SUCCEEDED(controller->get_CoreWebView2(&webView)) && webView)
        {
            script = webView->AddScriptToExecuteOnDocumentCreated(PanelScript, nullptr);
            webView->ExecuteScript(PanelScript, nullptr); // a document that already exists
        }
        Log(L"teams web: controller %p, transparent background %08X, panel script %08X", controller, background, script);
    }

    ComPtr<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> Wrap(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* inner)
    {
        ComPtr<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> keep(inner);
        return Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [keep](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                if (SUCCEEDED(result)) Prepare(controller);
                return keep ? keep->Invoke(result, controller) : S_OK;
            });
    }

    ComPtr<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler> Wrap(ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler* inner)
    {
        ComPtr<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler> keep(inner);
        return Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
            [keep](HRESULT result, ICoreWebView2CompositionController* composition) -> HRESULT {
                ComPtr<ICoreWebView2Controller> controller;
                if (SUCCEEDED(result) && composition && SUCCEEDED(composition->QueryInterface(IID_PPV_ARGS(&controller)))) Prepare(controller.Get());
                return keep ? keep->Invoke(result, composition) : S_OK;
            });
    }

    HRESULT STDMETHODCALLTYPE Hook_CreateController(void* self, HWND parent, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* handler)
    {
        return Real_CreateController(self, parent, Wrap(handler).Get());
    }
    HRESULT STDMETHODCALLTYPE Hook_CreateControllerBase(void* self, HWND parent, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* handler)
    {
        return Real_CreateControllerBase(self, parent, Wrap(handler).Get());
    }
    HRESULT STDMETHODCALLTYPE Hook_CreateComposition(void* self, HWND parent, ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler* handler)
    {
        return Real_CreateComposition(self, parent, Wrap(handler).Get());
    }
    HRESULT STDMETHODCALLTYPE Hook_CreateControllerWithOptions(void* self, HWND parent, void* options, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* handler)
    {
        return Real_CreateControllerWithOptions(self, parent, options, Wrap(handler).Get());
    }
    HRESULT STDMETHODCALLTYPE Hook_CreateCompositionWithOptions(void* self, HWND parent, void* options, ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler* handler)
    {
        return Real_CreateCompositionWithOptions(self, parent, options, Wrap(handler).Get());
    }

    void PatchEnvironment(ICoreWebView2Environment* environment)
    {
        if (!environment || g_environmentPatched) return;
        g_environmentPatched = true;
        ComPtr<ICoreWebView2Environment10> latest;
        const bool hasLatest = SUCCEEDED(environment->QueryInterface(IID_PPV_ARGS(&latest)));
        void** vtable = *reinterpret_cast<void***>(hasLatest ? static_cast<IUnknown*>(latest.Get()) : environment);
        Real_CreateController = reinterpret_cast<CreateControllerFn>(vtable[3]);
        PatchSlot(&vtable[3], reinterpret_cast<void*>(&Hook_CreateController));
        if (hasLatest)
        {
            Real_CreateComposition = reinterpret_cast<CreateCompositionFn>(vtable[9]);
            PatchSlot(&vtable[9], reinterpret_cast<void*>(&Hook_CreateComposition));
            Real_CreateControllerWithOptions = reinterpret_cast<CreateControllerWithOptionsFn>(vtable[21]);
            PatchSlot(&vtable[21], reinterpret_cast<void*>(&Hook_CreateControllerWithOptions));
            Real_CreateCompositionWithOptions = reinterpret_cast<CreateCompositionWithOptionsFn>(vtable[22]);
            PatchSlot(&vtable[22], reinterpret_cast<void*>(&Hook_CreateCompositionWithOptions));
        }
        void** baseTable = *reinterpret_cast<void***>(environment);
        if (baseTable != vtable)
        {
            Real_CreateControllerBase = reinterpret_cast<CreateControllerFn>(baseTable[3]);
            PatchSlot(&baseTable[3], reinterpret_cast<void*>(&Hook_CreateControllerBase));
        }
        Log(L"teams web: environment %p watched (%s)", environment, hasLatest ? L"all controller kinds" : L"plain controllers only");
    }

    HRESULT STDAPICALLTYPE Hook_CreateEnvironment(PCWSTR browserFolder, PCWSTR userDataFolder, ICoreWebView2EnvironmentOptions* options,
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler)
    {
        ComPtr<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> keep(handler);
        auto wrapped = Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [keep](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                if (SUCCEEDED(result)) PatchEnvironment(environment);
                return keep ? keep->Invoke(result, environment) : S_OK;
            });
        Log(L"teams web: environment requested");
        return Real_CreateEnvironment(browserFolder, userDataFolder, options, wrapped.Get());
    }
}

namespace
{
    SRWLOCK g_loaderLock = SRWLOCK_INIT;
    bool g_loaderHooked = false;

    // Detours the loader's CreateCoreWebView2EnvironmentWithOptions once it is in the process.
    void HookLoader(HMODULE loader)
    {
        AcquireSRWLockExclusive(&g_loaderLock);
        if (!g_loaderHooked && loader)
        {
            if (auto real = reinterpret_cast<CreateEnvironmentFn>(GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions")))
            {
                Real_CreateEnvironment = real;
                DetourTransactionBegin();
                DetourUpdateThread(GetCurrentThread());
                DetourAttach(reinterpret_cast<PVOID*>(&Real_CreateEnvironment), reinterpret_cast<PVOID>(&Hook_CreateEnvironment));
                const LONG result = DetourTransactionCommit();
                g_loaderHooked = result == NO_ERROR;
                Log(L"teams web: loader %p, environment detour %ld", loader, result);
            }
        }
        ReleaseSRWLockExclusive(&g_loaderLock);
    }

    bool IsLoader(HMODULE module)
    {
        wchar_t path[MAX_PATH]{};
        if (!module || !GetModuleFileNameW(module, path, MAX_PATH)) return false;
        const wchar_t* name = wcsrchr(path, L'\\');
        return _wcsicmp(name ? name + 1 : path, L"WebView2Loader.dll") == 0;
    }

    // Teams loads WebView2Loader.dll on demand (delay load), after the hook arrives: watch library loads until it does.
    decltype(&LoadLibraryExW) Real_LoadLibraryExW = LoadLibraryExW;
    decltype(&LoadLibraryExA) Real_LoadLibraryExA = LoadLibraryExA;
    HMODULE WINAPI Hook_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags)
    {
        const HMODULE module = Real_LoadLibraryExW(name, file, flags);
        if (!g_loaderHooked && IsLoader(module)) HookLoader(module);
        return module;
    }
    HMODULE WINAPI Hook_LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags)
    {
        const HMODULE module = Real_LoadLibraryExA(name, file, flags);
        if (!g_loaderHooked && IsLoader(module)) HookLoader(module);
        return module;
    }
}

void StartTeamsWebGlass()
{
    // Called from DllMain. Already mapped (a static import): hook it now; otherwise when Teams loads it.
    if (HMODULE loader = GetModuleHandleW(L"WebView2Loader.dll")) { HookLoader(loader); return; }
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&Real_LoadLibraryExW), reinterpret_cast<PVOID>(&Hook_LoadLibraryExW));
    DetourAttach(reinterpret_cast<PVOID*>(&Real_LoadLibraryExA), reinterpret_cast<PVOID>(&Hook_LoadLibraryExA));
    Log(L"teams web: waiting for WebView2Loader.dll (%ld)", DetourTransactionCommit());
}
