// WebView2 apps with "Glass inside the app" (Teams, the new Outlook): instead of keying colors out of the finished frame (which can't tell a dark photo
// pixel from a dark panel), the page itself leaves its big panels clear. The hook is in the app from launch, so it
// catches CreateCoreWebView2EnvironmentWithOptions (WebView2Loader.dll) and every web view the app then creates:
// - the controller's default background becomes transparent, so Chromium draws real alpha where nothing is painted;
// - a script, run in every document, clears the background of large, square, dark, neutral panels and bars only
//   (the app rail, the title and header bars, the chat list, the conversation), and makes thin grey divider lines
//   (panel edges, separators, splitters) transparent. Text, message bubbles, buttons, accent lines, images and video are never touched.
// The DXGI hook in the GPU process then passes that alpha through (GLASSFORGE_DXGI_TUNING gain 0, dxgiglass.cpp).
#include <windows.h>
#include <cstdio>
#include <algorithm>
#include <map>
#include <string>
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
  // Thin grey lines (dividers, panel seams, splitters) read as seams on glass, so they go too. Accent-colored lines
  // (the selected tab, the compose box focus) are not grey and stay.
  const LINE = 'gf-noline';
  const css = `.${LINE}, .${LINE}::before, .${LINE}::after { border-color: transparent !important; }
.${LINE}::before, .${LINE}::after { background-color: transparent !important; box-shadow: none !important; }`;
  // Only writes when the value differs, so the page's MutationObserver below doesn't wake itself up.
  const set = (el, prop, value) => {
    if (el.style.getPropertyValue(prop) !== value || el.style.getPropertyPriority(prop) !== 'important')
      el.style.setProperty(prop, value, 'important');
  };
  const mark = (el) => { if (!el.classList.contains(LINE)) el.classList.add(LINE); };
  const rgba = (text) => {
    const m = /rgba?\(([^)]+)\)/.exec(text || '');
    if (!m) return null;
    const [r, g, b, a = 1] = m[1].split(/[\s,\/]+/).filter(Boolean).map(Number);
    return { r, g, b, a };
  };
  const grey = (text) => { const c = rgba(text); return !!c && c.a > 0 && Math.max(c.r, c.g, c.b) - Math.min(c.r, c.g, c.b) <= 16; };
  const sides = ['top', 'right', 'bottom', 'left'];
  const unline = (el, style) => {
    for (const side of sides) {
      const width = parseFloat(style.getPropertyValue(`border-${side}-width`));
      if (width > 0 && width <= 2 && style.getPropertyValue(`border-${side}-style`) !== 'none' && grey(style.getPropertyValue(`border-${side}-color`)))
        set(el, `border-${side}-color`, 'transparent');
    }
    // Hairline shadows (0 1px 0 ..., inset -1px 0 0 ...) are drawn lines too; real drop shadows have blur.
    const shadow = style.boxShadow;
    if (shadow && shadow !== 'none') {
      const lengths = (shadow.match(/-?\d*\.?\d+px/g) || []).map(v => Math.abs(parseFloat(v)));
      if (lengths.length && Math.max(...lengths) <= 2) set(el, 'box-shadow', 'none');
    }
  };
  // Fluent draws some dividers and splitter lines with ::before/::after, which inline styles can't reach.
  const pseudoLine = (el) => ['::before', '::after'].some(p => {
    const s = getComputedStyle(el, p);
    if (s.content === 'none' || s.content === 'normal') return false;
    return (s.backgroundImage === 'none' && grey(s.backgroundColor))
      || sides.some(side => parseFloat(s.getPropertyValue(`border-${side}-width`)) > 0 && grey(s.getPropertyValue(`border-${side}-color`)));
  });
  const clear = () => {
    const root = document.documentElement;
    if (!root) return;
    root.style.setProperty('background', 'transparent', 'important');
    if (document.body) document.body.style.setProperty('background', 'transparent', 'important');
    if (document.head && !document.getElementById('gf-noline-style')) {
      const sheet = document.createElement('style');
      sheet.id = 'gf-noline-style';
      sheet.textContent = css;
      document.head.appendChild(sheet);
    }
    for (const el of document.querySelectorAll('hr, [role="separator"]')) mark(el);
    const vw = innerWidth, vh = innerHeight;
    for (const el of document.querySelectorAll('body *')) {
      if (media.has(el.tagName.toUpperCase())) continue;
      const box = el.getBoundingClientRect();
      // Lines: a few pixels across and a real length along (dividers, splitters, panel seams). Scrollbar thumbs are
      // wider than 2 px, so only their borders could go, never the thumb itself.
      if ((box.height > 0 && box.height <= 8 && box.width >= vw * 0.08) || (box.width > 0 && box.width <= 8 && box.height >= vh * 0.08)) {
        const style = getComputedStyle(el);
        if (Math.min(box.width, box.height) <= 2 && style.backgroundImage === 'none' && grey(style.backgroundColor))
          set(el, 'background-color', 'transparent');
        unline(el, style);
        if (pseudoLine(el)) mark(el);
        continue;
      }
      if (box.width < vw * 0.15 && box.height < vh * 0.15) continue;
      const style = getComputedStyle(el);
      // Message bubbles, buttons, cards and the search box have rounded corners; panels and bars are square, or rounded
      // but large (Outlook's folder and reading panes) or nearly full-width (its ribbon).
      const rounded = parseFloat(style.borderTopLeftRadius) > 2 || parseFloat(style.borderBottomRightRadius) > 2;
      if (rounded && box.width * box.height < vw * vh * 0.06 && box.width < vw * 0.6) continue;
      // Sections and panes: their edge lines go (the line under the header, between the chat list and the chat).
      unline(el, style);
      // Panels and bars only: a quarter of the width or a third of the height, and not tiny.
      if ((box.width < vw * 0.25 && box.height < vh * 0.35) || box.width * box.height < vw * vh * 0.01) continue;
      if (style.backgroundImage !== 'none') continue;
      const m = style.backgroundColor.match(/rgba?\(([^)]+)\)/);
      if (!m) continue;
      const [r, g, b, a = 1] = m[1].split(',').map(Number);
      if (a === 0) continue;
      // Dark, colorless surfaces (the theme's backgrounds), not accent or content colors.
      if (Math.max(r, g, b) > 64 || Math.max(r, g, b) - Math.min(r, g, b) > 10) continue;
      set(el, 'background-color', 'transparent');
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

    // Diagnostic (only while %LOCALAPPDATA%\GlassForge\teams-lines.txt exists): lists everything in the page that can
    // draw a line (borders, shadows, outlines, thin elements, gradients, ::before/::after) into hook.log. Read-only.
    constexpr wchar_t LineSurveyScript[] = LR"JS((() => {
  const vw = innerWidth, vh = innerHeight, out = [];
  const sides = ['top', 'right', 'bottom', 'left'];
  const visible = c => c && c !== 'transparent' && !/rgba\([^)]*,\s*0\)$/.test(c);
  const name = el => {
    const cls = typeof el.className === 'string' ? el.className : (el.className && el.className.baseVal) || '';
    return el.tagName.toLowerCase() + (el.id ? '#' + el.id : '') + (el.getAttribute('role') ? '[role=' + el.getAttribute('role') + ']' : '')
      + (el.getAttribute('data-tid') ? '[tid=' + el.getAttribute('data-tid') + ']' : '') + (cls.trim() ? '.' + cls.trim().split(/\s+/).slice(0, 4).join('.') : '');
  };
  const path = el => { const p = []; for (let e = el; e && e !== document.body && p.length < 3; e = e.parentElement) p.push(name(e)); return p.join(' < '); };
  const rect = b => `${Math.round(b.left)},${Math.round(b.top)} ${Math.round(b.width)}x${Math.round(b.height)}`;
  const lines = (s, b, prefix) => {
    const f = [];
    for (const side of sides) {
      const w = parseFloat(s.getPropertyValue(`border-${side}-width`));
      const len = side === 'top' || side === 'bottom' ? b.width : b.height;
      if (w > 0 && s.getPropertyValue(`border-${side}-style`) !== 'none' && visible(s.getPropertyValue(`border-${side}-color`)) && len >= 60)
        f.push(`${prefix}border-${side} ${w}px ${s.getPropertyValue(`border-${side}-color`)}`);
    }
    if (s.boxShadow && s.boxShadow !== 'none' && (b.width >= 60 || b.height >= 60)) f.push(`${prefix}shadow ${s.boxShadow.slice(0, 120)}`);
    if (parseFloat(s.outlineWidth) > 0 && s.outlineStyle !== 'none' && visible(s.outlineColor)) f.push(`${prefix}outline ${s.outlineWidth} ${s.outlineColor}`);
    if (/gradient/.test(s.backgroundImage) && (b.width >= 60 || b.height >= 60)) f.push(`${prefix}gradient ${s.backgroundImage.slice(0, 140)}`);
    return f;
  };
  for (const el of document.querySelectorAll('body *')) {
    const b = el.getBoundingClientRect();
    if (b.width <= 0 || b.height <= 0 || b.right < 0 || b.bottom < 0 || b.left > vw || b.top > vh) continue;
    const s = getComputedStyle(el);
    const found = lines(s, b, '');
    if (((b.height <= 3 && b.width >= 60) || (b.width <= 3 && b.height >= 60)) && (visible(s.backgroundColor) || s.backgroundImage !== 'none'))
      found.push(`thin bg ${s.backgroundColor} ${s.backgroundImage.slice(0, 80)}`);
    for (const p of ['::before', '::after']) {
      const ps = getComputedStyle(el, p);
      if (ps.content === 'none' || ps.content === 'normal') continue;
      const pf = lines(ps, b, p + ' ');
      if (visible(ps.backgroundColor) || ps.backgroundImage !== 'none') pf.push(`${p} bg ${ps.backgroundColor} ${ps.backgroundImage.slice(0, 60)} size ${ps.width}x${ps.height} pos ${ps.position} ${ps.top},${ps.right},${ps.bottom},${ps.left}`);
      found.push(...pf);
    }
    if (found.length) out.push(`${rect(b)} ${path(el)} :: ${found.join(' | ')}`);
  }
  return `vw ${vw} vh ${vh} dpr ${devicePixelRatio} ${location.href.slice(0, 80)} items ${out.length}\n` + out.slice(0, 500).join('\n');
})())JS";

    bool SurveyRequested()
    {
        wchar_t path[MAX_PATH]{};
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH)) return false;
        wcscat_s(path, L"\\GlassForge\\teams-lines.txt");
        return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    }

    std::map<UINT_PTR, ComPtr<ICoreWebView2>> g_surveys;

    void LogSurvey(ICoreWebView2* webView, LPCWSTR json)
    {
        // The result is a JSON string: lines are separated by the two characters \n.
        std::wstring text = json ? json : L"(no result)";
        size_t start = 0;
        int count = 0;
        while (start < text.size() && count < 600)
        {
            size_t end = text.find(L"\\n", start);
            if (end == std::wstring::npos) end = text.size();
            for (size_t at = start; at < end; at += 900, ++count)
                Log(L"lines %p: %s", webView, text.substr(at, std::min<size_t>(900, end - at)).c_str());
            start = end + 2;
        }
    }

    void CALLBACK SurveyTimer(HWND, UINT, UINT_PTR id, DWORD)
    {
        KillTimer(nullptr, id);
        const auto it = g_surveys.find(id);
        if (it == g_surveys.end()) return;
        ComPtr<ICoreWebView2> webView = it->second;
        g_surveys.erase(it);
        webView->ExecuteScript(LineSurveyScript, Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [webView](HRESULT result, LPCWSTR json) -> HRESULT {
                if (FAILED(result)) Log(L"lines %p: survey failed %08X", webView.Get(), result);
                else LogSurvey(webView.Get(), json);
                return S_OK;
            }).Get());
    }

    void ScheduleSurvey(ICoreWebView2* webView)
    {
        if (!SurveyRequested()) return;
        for (const UINT delay : { 30000u, 90000u })
            if (const UINT_PTR id = SetTimer(nullptr, 0, delay, SurveyTimer)) g_surveys[id] = webView;
        Log(L"lines %p: survey scheduled", webView);
    }

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
            ScheduleSurvey(webView.Get());
        }
        Log(L"web glass: controller %p, transparent background %08X, panel script %08X", controller, background, script);
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
        Log(L"web glass: environment %p watched (%s)", environment, hasLatest ? L"all controller kinds" : L"plain controllers only");
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
        Log(L"web glass: environment requested");
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
                Log(L"web glass: loader %p, environment detour %ld", loader, result);
            }
        }
        ReleaseSRWLockExclusive(&g_loaderLock);
    }


    // The app loads WebView2Loader.dll after the hook arrives: on demand (Teams' delay load) or as a dependency of
    // another library (the new Outlook). Every load goes through ntdll's LdrLoadDll, so once one returns with the
    // loader in the process, it is hooked before the app can call it.
    using LdrLoadDllFn = LONG(NTAPI*)(PWSTR, PULONG, void*, PVOID*);
    LdrLoadDllFn Real_LdrLoadDll = nullptr;
    LONG NTAPI Hook_LdrLoadDll(PWSTR path, PULONG flags, void* name, PVOID* handle)
    {
        const LONG status = Real_LdrLoadDll(path, flags, name, handle);
        if (!g_loaderHooked)
            if (HMODULE loader = GetModuleHandleW(L"WebView2Loader.dll")) HookLoader(loader);
        return status;
    }
}

void StartWebViewGlass()
{
    // Called from DllMain. Already mapped (a static import): hook it now; otherwise when the app loads it.
    if (HMODULE loader = GetModuleHandleW(L"WebView2Loader.dll")) { HookLoader(loader); return; }
    Real_LdrLoadDll = reinterpret_cast<LdrLoadDllFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrLoadDll"));
    if (!Real_LdrLoadDll) return;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&Real_LdrLoadDll), reinterpret_cast<PVOID>(&Hook_LdrLoadDll));
    Log(L"web glass: waiting for WebView2Loader.dll (%ld)", DetourTransactionCommit());
}
