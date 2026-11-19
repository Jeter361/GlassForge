// Experimental: live glass for File Explorer the way Teams and Settings get it (docs/GLASS_WRITEUP.md, Update 7).
//
// Explorer's WinUI 3 content has no host backdrop brush (Microsoft.UI.Composition lacks it), but its top-level window
// is a plain Win32 window in this process. The system compositor (Windows.UI.Composition, which does have
// CreateHostBackdropBrush) can put a visual tree on that window through a desktop window target placed *below* the
// window's own content (isTopmost = FALSE). The tree is a live host backdrop (the blurred view of what is behind the
// window; it only renders once the window opts in with DWMWA_USE_HOSTBACKDROPBRUSH) with a dark tint over it, as in
// Teams. Explorer's content is already see-through (the paint hooks and xaml.cpp clear its backgrounds), so the tree
// shows wherever DWM's Acrylic showed before. The target composites above the window's own GDI surface, though, so the
// classic content (file list, navigation tree) is lifted into layered windows of its own (see Lift), and the glass
// leaves a hole where DWM draws the caption buttons (see Layout).
//
// All composition work runs on one thread of its own (its own DispatcherQueue and compositor), so Explorer's window
// threads are never touched.
//
// Config: %LOCALAPPDATA%\GlassForge\explorer-hostglass.txt, read per window. Tokens (any order):
//   red         a 50% red layer instead of the glass, to see whether the layer shows at all
//   AARRGGBB    the tint over the live blur (default 73181818, Teams' 45% #181818)
//   nobackdrop  also turn DWM's own Acrylic off (handled by the caller)
//   nohost      the tint alone over DWM's Acrylic, no live host backdrop
#include <windows.h>
#include <dwmapi.h>
#include <cwchar>
#include <algorithm>
#include <map>
#include <string>
#include <roapi.h>
#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>
#include <DispatcherQueue.h>
#undef GetCurrentTime
#include <windows.ui.composition.h>
#include <windows.ui.composition.desktop.h>
#include <windows.ui.composition.interop.h>
#include "hostglass.h"
#include "log.h"

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;
using namespace ABI::Windows::UI::Composition;
using namespace ABI::Windows::UI::Composition::Desktop;

namespace
{
    constexpr UINT AttachMessage = WM_APP + 0x47;
    constexpr UINT DetachMessage = WM_APP + 0x48;

    struct Config
    {
        bool on = false;
        bool red = false;
        bool noBackdrop = false;
        bool noHost = false;
        UINT32 tint = 0x73181818;
    };

    Config ReadConfig()
    {
        Config config;
        wchar_t path[MAX_PATH]{};
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH)) return config;
        wcscat_s(path, L"\\GlassForge\\explorer-hostglass.txt");
        const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE) return config;
        config.on = true;
        char text[256]{};
        DWORD read = 0;
        ReadFile(file, text, sizeof text - 1, &read, nullptr);
        CloseHandle(file);
        char* context = nullptr;
        for (char* token = strtok_s(text, " \t\r\n,", &context); token; token = strtok_s(nullptr, " \t\r\n,", &context))
        {
            if (_stricmp(token, "red") == 0) config.red = true;
            else if (_stricmp(token, "nobackdrop") == 0) config.noBackdrop = true;
            else if (_stricmp(token, "nohost") == 0) config.noHost = true;
            else if (strlen(token) == 8)
            {
                char* end = nullptr;
                const unsigned long value = strtoul(token, &end, 16);
                if (end && *end == 0) config.tint = static_cast<UINT32>(value);
            }
        }
        return config;
    }

    Config g_config;
    DWORD g_threadId = 0;
    INIT_ONCE g_start = INIT_ONCE_STATIC_INIT;

    // Composition thread only.
    ComPtr<ABI::Windows::System::IDispatcherQueueController> g_queue;
    ComPtr<ICompositor> g_compositor;
    // The glass is three rectangles around the caption buttons: DWM draws those buttons beneath this target, so a
    // single full-window layer covered them. Below the button row (full width), left of the buttons, right of them.
    struct Glass
    {
        ComPtr<IDesktopWindowTarget> target;
        ComPtr<IVisual> regions[3];
        RECT buttons{ -1, -1, -1, -1 }; // in client coordinates, last laid out
        LONG width = -1;
    };
    std::map<HWND, Glass> g_glass;
    HWINEVENTHOOK g_showHook = nullptr;
    constexpr UINT_PTR LayoutTimer = 1;

    // Explorer's classic content (the DirectUI file list and the navigation tree) is drawn into the top-level window's
    // own surface, which composites *below* this target, so the glass covered it. As a layered window of its own (black,
    // the colour the paint hooks leave where Explorer's background was, keyed out) it composites above, like the XAML
    // islands do. DUIViewWndClassName holds both, one per tab; Explorer makes new ones on navigation and new tabs.
    constexpr wchar_t ContentClass[] = L"DUIViewWndClassName";

    bool IsContent(HWND window)
    {
        wchar_t name[64]{};
        return GetClassNameW(window, name, _countof(name)) && wcscmp(name, ContentClass) == 0;
    }

    // A colour-keyed layered window is click-through wherever it is keyed out, so over empty background the wheel and
    // clicks fell through to the tab window underneath (scrolling only worked over file names). That window (the lifted
    // one's parent) gets a window procedure that hands mouse input to the deepest child under the cursor instead.
    SRWLOCK g_forwardLock = SRWLOCK_INIT;
    std::map<HWND, WNDPROC> g_forwarders;

    HWND DeepestChild(HWND parent, POINT screen)
    {
        HWND current = parent;
        for (int depth = 0; depth < 16; ++depth)
        {
            POINT point = screen;
            ScreenToClient(current, &point);
            const HWND child = ChildWindowFromPointEx(current, point, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
            if (!child || child == current) break;
            current = child;
        }
        return current;
    }

    LRESULT CALLBACK ForwardMouse(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        AcquireSRWLockShared(&g_forwardLock);
        const auto it = g_forwarders.find(window);
        const WNDPROC original = it != g_forwarders.end() ? it->second : nullptr;
        ReleaseSRWLockShared(&g_forwardLock);
        if (!original) return DefWindowProcW(window, message, wParam, lParam);
        if (message == WM_NCDESTROY)
        {
            SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
            AcquireSRWLockExclusive(&g_forwardLock);
            g_forwarders.erase(window);
            ReleaseSRWLockExclusive(&g_forwardLock);
            return CallWindowProcW(original, window, message, wParam, lParam);
        }
        // Explorer takes the mouse as pointer messages too (WM_POINTERUPDATE..WM_POINTERHWHEEL, screen coordinates).
        const bool pointer = message >= 0x0245 /* WM_POINTERUPDATE */ && message <= 0x024F /* WM_POINTERHWHEEL */
            && message != 0x024B /* WM_POINTERACTIVATE */ && message != 0x024C /* WM_POINTERCAPTURECHANGED */;
        const bool mouse = message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
        if (pointer || mouse)
        {
            static LONG logged[0x60]{};
            const UINT slot = mouse ? message - WM_MOUSEFIRST : 0x20 + (message - 0x0245);
            if (slot < _countof(logged) && InterlockedExchange(&logged[slot], 1) == 0)
                Log(L"host glass: tab window %p receives message %04X", window, message);
        }
        if ((pointer || mouse) && GetCapture() != window)
        {
            const bool wheel = pointer || message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL; // screen coordinates
            POINT screen{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
            if (!wheel) ClientToScreen(window, &screen);
            const HWND target = DeepestChild(window, screen);
            if (target && target != window)
            {
                POINT point = screen;
                if (!wheel) ScreenToClient(target, &point);
                return SendMessageW(target, message, wParam, wheel ? lParam : MAKELPARAM(point.x, point.y));
            }
        }
        return CallWindowProcW(original, window, message, wParam, lParam);
    }

    void ForwardFrom(HWND parent, bool on)
    {
        if (!parent) return;
        AcquireSRWLockExclusive(&g_forwardLock);
        const auto it = g_forwarders.find(parent);
        if (on && it == g_forwarders.end())
        {
            const auto original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(parent, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ForwardMouse)));
            if (original) g_forwarders[parent] = original;
        }
        else if (!on && it != g_forwarders.end()
            && GetWindowLongPtrW(parent, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(&ForwardMouse))
        {
            // Only when nobody subclassed it after us; otherwise ours stays in the chain and passes everything on.
            SetWindowLongPtrW(parent, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(it->second));
            g_forwarders.erase(it);
        }
        ReleaseSRWLockExclusive(&g_forwardLock);
    }

    void Lift(HWND window, bool on)
    {
        ForwardFrom(GetParent(window), on);
        const LONG_PTR style = GetWindowLongPtrW(window, GWL_EXSTYLE);
        if (on)
        {
            if (!(style & WS_EX_LAYERED)) SetWindowLongPtrW(window, GWL_EXSTYLE, style | WS_EX_LAYERED);
            SetLayeredWindowAttributes(window, RGB(0, 0, 0), 255, LWA_COLORKEY);
        }
        else if (style & WS_EX_LAYERED) SetWindowLongPtrW(window, GWL_EXSTYLE, style & ~static_cast<LONG_PTR>(WS_EX_LAYERED));
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    }

    void LiftAll(HWND root, bool on)
    {
        struct Args { bool on; int count; } args{ on, 0 };
        EnumChildWindows(root, [](HWND child, LPARAM param) -> BOOL {
            auto& a = *reinterpret_cast<Args*>(param);
            if (IsContent(child)) { Lift(child, a.on); ++a.count; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&args));
        Log(L"host glass: window %p content %s (%d)", root, on ? L"lifted" : L"put back", args.count);
    }

    // New content windows (another folder, another tab) are lifted the moment they are created or shown.
    void CALLBACK ContentShown(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG, DWORD, DWORD)
    {
        if (object != OBJID_WINDOW || !window || !IsContent(window)) return;
        if (g_glass.count(GetAncestor(window, GA_ROOT))) Lift(window, true);
    }

    ComPtr<IVisual> Fill(ICompositionBrush* brush)
    {
        ComPtr<ISpriteVisual> sprite;
        if (FAILED(g_compositor->CreateSpriteVisual(&sprite))) return nullptr;
        sprite->put_Brush(brush);
        ComPtr<IVisual2> sized;
        if (SUCCEEDED(sprite.As(&sized))) sized->put_RelativeSizeAdjustment({ 1.f, 1.f });
        ComPtr<IVisual> visual;
        sprite.As(&visual);
        return visual;
    }

    ComPtr<ICompositionBrush> Color(UINT32 argb)
    {
        ComPtr<ICompositionColorBrush> color;
        const ABI::Windows::UI::Color value{ BYTE(argb >> 24), BYTE(argb >> 16), BYTE(argb >> 8), BYTE(argb) };
        if (FAILED(g_compositor->CreateColorBrushWithColor(value, &color))) return nullptr;
        ComPtr<ICompositionBrush> brush;
        color.As(&brush);
        return brush;
    }

    // One glass rectangle: the live backdrop with the tint over it, both filling the rectangle.
    ComPtr<IVisual> Region(ICompositionBrush* backdrop, ICompositionBrush* tint)
    {
        ComPtr<IContainerVisual> container;
        ComPtr<IVisualCollection> children;
        if (FAILED(g_compositor->CreateContainerVisual(&container)) || FAILED(container->get_Children(&children))) return nullptr;
        if (backdrop) if (const auto visual = Fill(backdrop)) children->InsertAtTop(visual.Get());
        if (tint) if (const auto visual = Fill(tint)) children->InsertAtTop(visual.Get());
        ComPtr<IVisual> visual;
        container.As(&visual);
        return visual;
    }

    void Place(IVisual* visual, float x, float y, float relX, float w, float h, float relW, float relH)
    {
        if (!visual) return;
        visual->put_Offset({ x, y, 0.f });
        visual->put_Size({ w, h });
        ComPtr<IVisual2> visual2;
        if (SUCCEEDED(visual->QueryInterface(IID_PPV_ARGS(&visual2))))
        {
            visual2->put_RelativeOffsetAdjustment({ relX, 0.f, 0.f });
            visual2->put_RelativeSizeAdjustment({ relW, relH });
        }
    }

    // The caption buttons' rectangle, in the target's (client) coordinates; empty when DWM draws none.
    RECT CaptionButtons(HWND window)
    {
        RECT buttons{};
        if (FAILED(DwmGetWindowAttribute(window, DWMWA_CAPTION_BUTTON_BOUNDS, &buttons, sizeof buttons)) || IsRectEmpty(&buttons)) return RECT{};
        // DWM gives them relative to the window's top-left corner; the target starts at the client area's.
        RECT frame{};
        POINT client{};
        GetWindowRect(window, &frame);
        ClientToScreen(window, &client);
        OffsetRect(&buttons, frame.left - client.x, frame.top - client.y);
        return buttons;
    }

    // Lays the three rectangles out around the buttons. Widths follow the window by themselves (relative sizes); only
    // the buttons' own position (maximize, DPI) needs a new layout, checked on a timer.
    void Layout(HWND window, Glass& glass)
    {
        const RECT buttons = CaptionButtons(window);
        RECT client{};
        GetClientRect(window, &client);
        if (EqualRect(&buttons, &glass.buttons) && client.right == glass.width) return;
        glass.buttons = buttons;
        glass.width = client.right;
        const float below = float(std::max<LONG>(buttons.bottom, 0));
        const float leftGap = IsRectEmpty(&buttons) ? 0.f : float(client.right - buttons.left); // from the right edge
        const float rightGap = IsRectEmpty(&buttons) ? 0.f : float(std::max<LONG>(client.right - buttons.right, 0));
        Place(glass.regions[0].Get(), 0, below, 0, 0, -below, 1, 1);           // everything under the button row
        Place(glass.regions[1].Get(), 0, 0, 0, -leftGap, below, 1, 0);         // the row, left of the buttons
        Place(glass.regions[2].Get(), -rightGap, 0, 1, rightGap, below, 0, 0); // the row, right of them (a pixel or two)
    }

    void Attach(HWND window, const Config& config)
    {
        if (!g_compositor || g_glass.count(window) || !IsWindow(window)) return;
        Glass glass;
        ComPtr<ICompositorDesktopInterop> interop;
        HRESULT hr = g_compositor.As(&interop);
        if (SUCCEEDED(hr)) hr = interop->CreateDesktopWindowTarget(window, FALSE, &glass.target);
        if (FAILED(hr)) { Log(L"host glass: window %p target FAILED %08X", window, hr); return; }

        ComPtr<IContainerVisual> container;
        ComPtr<IVisualCollection> children;
        hr = g_compositor->CreateContainerVisual(&container);
        if (SUCCEEDED(hr)) hr = container->get_Children(&children);
        if (FAILED(hr)) { Log(L"host glass: window %p visuals FAILED %08X", window, hr); return; }
        ComPtr<IVisual2> containerSize;
        if (SUCCEEDED(container.As(&containerSize))) containerSize->put_RelativeSizeAdjustment({ 1.f, 1.f });

        HRESULT backdrop = S_FALSE;
        ComPtr<ICompositionBrush> backdropBrush;
        if (!config.red && !config.noHost)
        {
            ComPtr<ICompositor3> compositor3;
            ComPtr<ICompositionBackdropBrush> host;
            backdrop = g_compositor.As(&compositor3);
            if (SUCCEEDED(backdrop)) backdrop = compositor3->CreateHostBackdropBrush(&host);
            if (SUCCEEDED(backdrop)) backdrop = host.As(&backdropBrush);
        }
        const auto tint = Color(config.red ? 0x80FF0000 : config.tint);
        for (auto& region : glass.regions)
            if ((region = Region(backdropBrush.Get(), tint.Get()))) children->InsertAtTop(region.Get());
        Layout(window, glass);

        ComPtr<ICompositionTarget> compositionTarget;
        ComPtr<IVisual> root;
        hr = glass.target.As(&compositionTarget);
        if (SUCCEEDED(hr)) hr = container.As(&root);
        if (SUCCEEDED(hr)) hr = compositionTarget->put_Root(root.Get());
        Log(L"host glass: window %p %s layer %08X (host backdrop %08X, tint %08X, buttons %ld,%ld-%ld,%ld)", window,
            config.red ? L"red test" : L"glass", hr, backdrop, config.red ? 0x80FF0000u : config.tint, glass.buttons.left, glass.buttons.top,
            glass.buttons.right, glass.buttons.bottom);
        if (FAILED(hr)) return;
        g_glass[window] = std::move(glass);
        LiftAll(window, true);
    }

    void Detach(HWND window)
    {
        const auto it = g_glass.find(window);
        if (it == g_glass.end()) return;
        ComPtr<ICompositionTarget> target;
        if (SUCCEEDED(it->second.target.As(&target))) target->put_Root(nullptr);
        g_glass.erase(it);
        if (IsWindow(window)) LiftAll(window, false);
        Log(L"host glass: window %p layer removed", window);
    }

    void Tick()
    {
        for (auto it = g_glass.begin(); it != g_glass.end();)
        {
            if (!IsWindow(it->first)) { it = g_glass.erase(it); continue; }
            Layout(it->first, it->second);
            ++it;
        }
    }

    DWORD WINAPI CompositionThread(void* ready)
    {
        RoInitialize(RO_INIT_SINGLETHREADED);
        MSG message;
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE); // the thread's message queue, before anyone posts to it
        const DispatcherQueueOptions options{ sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA };
        HRESULT hr = CreateDispatcherQueueController(options, reinterpret_cast<PDISPATCHERQUEUECONTROLLER*>(g_queue.ReleaseAndGetAddressOf()));
        ComPtr<IInspectable> instance;
        if (SUCCEEDED(hr)) hr = RoActivateInstance(HStringReference(RuntimeClass_Windows_UI_Composition_Compositor).Get(), &instance);
        if (SUCCEEDED(hr)) hr = instance.As(&g_compositor);
        g_showHook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, nullptr, ContentShown, GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
        const UINT_PTR timer = SetTimer(nullptr, LayoutTimer, 500, nullptr);
        Log(L"host glass: compositor thread %lu %08X (content watch %s)", GetCurrentThreadId(), hr, g_showHook ? L"on" : L"FAILED");
        SetEvent(static_cast<HANDLE>(ready));
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            if (!message.hwnd && message.message == AttachMessage) Attach(reinterpret_cast<HWND>(message.wParam), g_config);
            else if (!message.hwnd && message.message == DetachMessage) Detach(reinterpret_cast<HWND>(message.wParam));
            else if (!message.hwnd && message.message == WM_TIMER && message.wParam == timer) Tick();
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return 0;
    }

    BOOL CALLBACK StartThread(PINIT_ONCE, PVOID, PVOID*)
    {
        const HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ready) return FALSE;
        if (const HANDLE thread = CreateThread(nullptr, 0, CompositionThread, ready, 0, &g_threadId))
        {
            WaitForSingleObject(ready, 5000);
            CloseHandle(thread);
        }
        CloseHandle(ready);
        return g_threadId != 0;
    }
}

bool ApplyExplorerHostGlass(HWND root)
{
    const Config config = ReadConfig();
    if (!config.on) return false;
    g_config = config;
    // The host backdrop only renders for windows that opt in, and only the window's own process can opt it in.
    const BOOL on = TRUE;
    const HRESULT optIn = DwmSetWindowAttribute(root, DWMWA_USE_HOSTBACKDROPBRUSH, &on, sizeof on);
    if (!InitOnceExecuteOnce(&g_start, StartThread, nullptr, nullptr) || !g_threadId) { Log(L"host glass: no compositor thread"); return false; }
    PostThreadMessageW(g_threadId, AttachMessage, reinterpret_cast<WPARAM>(root), 0);
    Log(L"host glass: window %p opted in %08X%s", root, optIn, config.noBackdrop ? L", DWM backdrop off" : L"");
    return config.noBackdrop;
}

void RemoveExplorerHostGlass(HWND root)
{
    if (g_threadId) PostThreadMessageW(g_threadId, DetachMessage, reinterpret_cast<WPARAM>(root), 0);
}
