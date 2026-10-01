// GlassForge.Hook: loaded into one target app through a thread-scoped SetWindowsHookEx, so only the windows
// GlassForge was asked to style are affected.
//
// How the glass works: the hooked window gets an Acrylic system backdrop with its frame extended over the whole
// client area. Inside an extended frame DWM treats black as see-through, so every fill in one of the app's
// background colors is swapped for black and the backdrop shows there; everything else paints normally.
// The survey counters (dumped on request) show which calls paint which colors, to pick the background colors.
#include <windows.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <vssym32.h>
#include <intrin.h>
#include <d2d1.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <map>
#include <string>
#include "detours.h"
#include "log.h"
#include "xaml.h"

namespace
{
    HMODULE g_module = nullptr;
    // Detours are process-wide and shared by every hooked window; each Explorer window runs on its own thread
    // and keeps its own state in thread-locals below.
    SRWLOCK g_installLock = SRWLOCK_INIT;
    bool g_installed = false;
    UINT g_dumpMessage = 0;
    UINT g_detachMessage = 0;
    UINT g_attachMessage = 0;
    // Survey counters are a development aid: they cost a string per paint call, so they only run when
    // %LOCALAPPDATA%\GlassForge\hook.survey exists at install time.
    bool g_survey = false;
    SRWLOCK g_statsLock = SRWLOCK_INIT;
    std::map<std::wstring, unsigned> g_stats;

    // Explorer's dark-mode backgrounds, from the survey: main fill, details pane, and navigation pane.
    constexpr COLORREF BackgroundColors[] = { RGB(0x19, 0x19, 0x19), RGB(0x1C, 0x1C, 0x1C), RGB(0x13, 0x13, 0x13) };
    HBRUSH g_black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    // Styled windows and the backdrop each one asked for, to give back on detach.
    SRWLOCK g_rootLock = SRWLOCK_INIT;
    std::map<HWND, int> g_roots;
    thread_local HWND t_root = nullptr;
    thread_local bool t_seen = false;
    thread_local bool t_detached = false;
    using ThemeClassQuery = HRESULT(WINAPI*)(HTHEME, LPWSTR, int);
    ThemeClassQuery g_themeClassQuery = nullptr;
    // Memory DCs have no HWND. Propagate ownership from their source DC instead
    // of treating every drawing call in Explorer's process as ours.
    SRWLOCK g_dcLock = SRWLOCK_INIT;
    std::map<HDC, bool> g_ownedDCs;

    bool IsRoot(HWND window)
    {
        AcquireSRWLockShared(&g_rootLock);
        const bool found = g_roots.count(window) != 0;
        ReleaseSRWLockShared(&g_rootLock);
        return found;
    }

    bool IsTargetDC(HDC dc)
    {
        if (!t_root) return false;
        if (const HWND window = WindowFromDC(dc))
            return GetAncestor(window, GA_ROOT) == t_root;
        AcquireSRWLockShared(&g_dcLock);
        const auto entry = g_ownedDCs.find(dc);
        const bool owned = entry != g_ownedDCs.end() && entry->second;
        ReleaseSRWLockShared(&g_dcLock);
        return owned;
    }

    bool IsBackground(COLORREF color)
    {
        for (const auto background : BackgroundColors) if (color == background) return true;
        return false;
    }

    COLORREF BrushColor(HBRUSH brush, bool& solid)
    {
        auto value = reinterpret_cast<ULONG_PTR>(brush);
        solid = true;
        if (value >= 1 && value <= 31) return GetSysColor(static_cast<int>(value - 1));
        LOGBRUSH info{};
        if (GetObjectW(brush, sizeof info, &info) == sizeof info && info.lbStyle == BS_SOLID) return info.lbColor;
        solid = false;
        return 0;
    }

    // GDI leaves the alpha byte at 0. Inside the extended frame DWM reads a pixel as premultiplied, so a colored
    // pixel with alpha 0 is *added* onto the backdrop: that is the glowing text and washed-out highlights. Text is
    // therefore drawn as an alpha mask and blended in, and colored fills are made opaque.
    std::array<BYTE, 256> g_coverageAlpha{};
    Microsoft::WRL::ComPtr<ID2D1Factory> g_d2d;

    void BuildCoverageAlpha()
    {
        // Lifts antialiased edges, which a straight linear mask renders too thin and grey over a dark backdrop.
        for (int i = 0; i < 256; ++i)
            g_coverageAlpha[i] = static_cast<BYTE>(std::pow(i / 255.0, 1 / 1.4) * 255 + 0.5);
    }

    decltype(&FillRect) Real_FillRect = FillRect;
    decltype(&CreateCompatibleDC) Real_CreateCompatibleDC = CreateCompatibleDC;
    decltype(&DeleteDC) Real_DeleteDC = DeleteDC;
    decltype(&PatBlt) Real_PatBlt = PatBlt;
    decltype(&ExtTextOutW) Real_ExtTextOutW = ExtTextOutW;
    decltype(&DrawThemeBackground) Real_DrawThemeBackground = DrawThemeBackground;
    decltype(&DrawThemeBackgroundEx) Real_DrawThemeBackgroundEx = DrawThemeBackgroundEx;
    decltype(&BitBlt) Real_BitBlt = BitBlt;
    decltype(&GdiAlphaBlend) Real_GdiAlphaBlend = GdiAlphaBlend;
    decltype(&DwmSetWindowAttribute) Real_DwmSetWindowAttribute = DwmSetWindowAttribute;
    decltype(&DwmExtendFrameIntoClientArea) Real_DwmExtendFrameIntoClientArea = DwmExtendFrameIntoClientArea;
    constexpr int Acrylic = 3;
    constexpr MARGINS WholeWindow{ -1, -1, -1, -1 };
    constexpr UINT_PTR SubclassId = 0x47467731; // "GFw1"
    constexpr UINT_PTR XamlTimerId = 0x47467831;

    HDC WINAPI Hook_CreateCompatibleDC(HDC source)
    {
        const bool owned = IsTargetDC(source);
        const HDC result = Real_CreateCompatibleDC(source);
        if (result && owned)
        {
            AcquireSRWLockExclusive(&g_dcLock);
            g_ownedDCs[result] = true;
            ReleaseSRWLockExclusive(&g_dcLock);
        }
        return result;
    }

    BOOL WINAPI Hook_DeleteDC(HDC dc)
    {
        const BOOL result = Real_DeleteDC(dc);
        if (result)
        {
            AcquireSRWLockExclusive(&g_dcLock);
            g_ownedDCs.erase(dc);
            ReleaseSRWLockExclusive(&g_dcLock);
        }
        return result;
    }

    // Explorer re-applies its own frame (Mica Alt, top-only margins) on activation and resize, which would leave our
    // black fills showing as plain black. While the window is ours, its requests are remembered but ours stay in force.
    HRESULT WINAPI Hook_DwmSetWindowAttribute(HWND window, DWORD attribute, LPCVOID value, DWORD size)
    {
        if (attribute == DWMWA_SYSTEMBACKDROP_TYPE && value && size == sizeof(int))
        {
            AcquireSRWLockExclusive(&g_rootLock);
            const auto entry = g_roots.find(window);
            const bool ours = entry != g_roots.end();
            if (ours) entry->second = *static_cast<const int*>(value);
            ReleaseSRWLockExclusive(&g_rootLock);
            if (ours) return Real_DwmSetWindowAttribute(window, attribute, &Acrylic, sizeof Acrylic);
        }
        return Real_DwmSetWindowAttribute(window, attribute, value, size);
    }

    HRESULT WINAPI Hook_DwmExtendFrameIntoClientArea(HWND window, const MARGINS* margins)
    {
        return Real_DwmExtendFrameIntoClientArea(window, IsRoot(window) ? &WholeWindow : margins);
    }

    // DWM swaps Acrylic for a flat fill on inactive windows; drawing the frame as active keeps the glass live.
    bool RemoveGlass(bool destroying = false);
    void Uninstall();

    LRESULT CALLBACK RootSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
    {
        if (message == WM_TIMER && wParam == XamlTimerId) { RefreshXamlStyling(); return 0; }
        // A closed window takes its thread with it and never sees Detach: forget it here, or the detours would
        // stay installed for a window that no longer exists.
        if (message == WM_NCDESTROY && window == t_root && !RemoveGlass(true)) Uninstall();
        if (message == WM_NCACTIVATE) return DefSubclassProc(window, message, TRUE, lParam);
        return DefSubclassProc(window, message, wParam, lParam);
    }

    std::wstring CallerModule(void* returnAddress)
    {
        HMODULE module = nullptr;
        wchar_t path[MAX_PATH] = L"?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCWSTR>(returnAddress), &module))
            GetModuleFileNameW(module, path, MAX_PATH);
        const wchar_t* name = wcsrchr(path, L'\\');
        return name ? name + 1 : path;
    }

    // A macro so the key string is only built while the survey runs.
#define Count(key) do { if (g_survey) CountCall(key); } while (false)
    void CountCall(const std::wstring& key)
    {
        AcquireSRWLockExclusive(&g_statsLock);
        ++g_stats[key];
        ReleaseSRWLockExclusive(&g_statsLock);
    }

    std::wstring RectText(LPCRECT rect);

    std::wstring ColorText(COLORREF color)
    {
        wchar_t text[16];
        swprintf_s(text, L"#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
        return text;
    }

    std::wstring BrushText(HBRUSH brush)
    {
        // FillRect accepts COLOR_xxx + 1 in place of a brush.
        auto value = reinterpret_cast<ULONG_PTR>(brush);
        if (value >= 1 && value <= 31) return L"syscolor" + std::to_wstring(value - 1) + L" " + ColorText(GetSysColor(static_cast<int>(value - 1)));
        LOGBRUSH info{};
        if (GetObjectW(brush, sizeof info, &info) == sizeof info && info.lbStyle == BS_SOLID) return ColorText(info.lbColor);
        return L"brush?";
    }

    // Alpha only means anything on 32-bit surfaces; masks and palettized bitmaps are left to plain GDI.
    bool HasAlphaChannel(HDC dc)
    {
        if (WindowFromDC(dc)) return true;
        BITMAP bitmap{};
        const auto selected = static_cast<HBITMAP>(GetCurrentObject(dc, OBJ_BITMAP));
        return selected && GetObjectW(selected, sizeof bitmap, &bitmap) && bitmap.bmBitsPixel == 32;
    }

    // Fills a rectangle with a solid color at full alpha, so it covers the backdrop instead of glowing over it.
    bool FillOpaque(HDC dc, const RECT* rect, COLORREF color)
    {
        if (!rect || IsRectEmpty(rect) || !HasAlphaChannel(dc)) return false;
        BP_PAINTPARAMS params{ sizeof params };
        HDC buffer = nullptr;
        const HPAINTBUFFER paint = BeginBufferedPaint(dc, rect, BPBF_TOPDOWNDIB, &params, &buffer);
        if (!paint) return false;
        SetDCBrushColor(buffer, color);
        Real_FillRect(buffer, rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        BufferedPaintMakeOpaque(paint, nullptr);
        return SUCCEEDED(EndBufferedPaint(paint, TRUE));
    }

    int WINAPI Hook_FillRect(HDC hdc, const RECT* rect, HBRUSH brush)
    {
        if (!IsTargetDC(hdc)) return Real_FillRect(hdc, rect, brush);
        Count(L"FillRect " + CallerModule(_ReturnAddress()) + L" " + BrushText(brush) + L" " + RectText(rect));
        bool solid = false;
        const COLORREF color = BrushColor(brush, solid);
        if (solid && IsBackground(color)) return Real_FillRect(hdc, rect, g_black);
        if (solid && color != RGB(0, 0, 0) && FillOpaque(hdc, rect, color)) return TRUE;
        return Real_FillRect(hdc, rect, brush);
    }

    BOOL WINAPI Hook_PatBlt(HDC hdc, int x, int y, int w, int h, DWORD rop)
    {
        if (!IsTargetDC(hdc)) return Real_PatBlt(hdc, x, y, w, h, rop);
        if (rop == PATCOPY) Count(L"PatBlt " + CallerModule(_ReturnAddress()) + L" " + ColorText(GetDCBrushColor(hdc)));
        bool solid = false;
        const HBRUSH brush = static_cast<HBRUSH>(GetCurrentObject(hdc, OBJ_BRUSH));
        const COLORREF color = brush == GetStockObject(DC_BRUSH) ? GetDCBrushColor(hdc) : BrushColor(brush, solid);
        if (brush == GetStockObject(DC_BRUSH)) solid = true;
        if (rop == PATCOPY && solid && IsBackground(color))
        {
            const HGDIOBJ previous = SelectObject(hdc, g_black);
            if (previous && previous != HGDI_ERROR)
            {
                const BOOL result = Real_PatBlt(hdc, x, y, w, h, rop);
                SelectObject(hdc, previous);
                return result;
            }
        }
        return Real_PatBlt(hdc, x, y, w, h, rop);
    }

    // Where the glyphs land, in the DC's logical coordinates, including any lpDx advances and the text alignment.
    bool TextBounds(HDC dc, int x, int y, UINT options, const RECT* rect, LPCWSTR text, UINT count, const INT* dx, RECT& bounds)
    {
        SIZE size{};
        const BOOL measured = options & ETO_GLYPH_INDEX
            ? GetTextExtentPointI(dc, const_cast<LPWORD>(reinterpret_cast<const WORD*>(text)), count, &size)
            : GetTextExtentPoint32W(dc, text, count, &size);
        if (!measured) return false;
        if (dx)
        {
            const UINT stride = options & ETO_PDY ? 2 : 1;
            LONG advance = 0, rise = 0;
            for (UINT i = 0; i < count; ++i)
            {
                advance += dx[i * stride];
                if (stride == 2) rise += dx[i * stride + 1];
            }
            size.cx = std::max(size.cx, advance);
            size.cy += std::abs(rise);
        }
        const UINT align = GetTextAlign(dc);
        if ((align & TA_BASELINE) == TA_BASELINE)
        {
            TEXTMETRICW metrics{};
            if (GetTextMetricsW(dc, &metrics)) y -= metrics.tmAscent;
        }
        else if ((align & TA_BOTTOM) == TA_BOTTOM) y -= size.cy;
        if ((align & TA_CENTER) == TA_CENTER) x -= size.cx / 2;
        else if ((align & TA_RIGHT) == TA_RIGHT) x -= size.cx;
        // Italic and ClearType glyphs reach a little past their advance width.
        const LONG overhang = size.cy / 4 + 2;
        SetRect(&bounds, x - overhang, y, x + size.cx + overhang, y + size.cy);
        if (rect && (options & ETO_CLIPPED)) IntersectRect(&bounds, &bounds, rect);
        return !IsRectEmpty(&bounds);
    }

    // Draws the glyphs white on black offscreen, turns their brightness into coverage alpha, and blends the text
    // color in with that alpha. The result looks like normal text over glass instead of light added onto it.
    bool DrawAlphaText(HDC dc, int x, int y, UINT options, const RECT* rect, LPCWSTR text, UINT count, const INT* dx)
    {
        RECT bounds{};
        if (!TextBounds(dc, x, y, options, rect, text, count, dx, bounds)) return false;
        BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        BP_PAINTPARAMS params{ sizeof params, BPPF_ERASE | BPPF_NOCLIP, nullptr, &blend };
        HDC buffer = nullptr;
        const HPAINTBUFFER paint = BeginBufferedPaint(dc, &bounds, BPBF_TOPDOWNDIB, &params, &buffer);
        if (!paint) return false;
        SelectObject(buffer, GetCurrentObject(dc, OBJ_FONT));
        SetTextAlign(buffer, GetTextAlign(dc));
        SetLayout(buffer, GetLayout(dc));
        SetTextCharacterExtra(buffer, GetTextCharacterExtra(dc));
        SetBkMode(buffer, TRANSPARENT);
        SetTextColor(buffer, RGB(255, 255, 255));
        RGBQUAD* pixels = nullptr;
        int stride = 0;
        RECT area{};
        if (!Real_ExtTextOutW(buffer, x, y, options & ~ETO_OPAQUE, rect, text, count, dx) ||
            FAILED(GetBufferedPaintBits(paint, &pixels, &stride)) || FAILED(GetBufferedPaintTargetRect(paint, &area)))
        {
            EndBufferedPaint(paint, FALSE);
            return false;
        }
        const COLORREF color = GetTextColor(dc) & 0xFFFFFF;
        const int red = GetRValue(color), green = GetGValue(color), blue = GetBValue(color);
        for (LONG row = 0; row < area.bottom - area.top; ++row)
        {
            RGBQUAD* pixel = pixels + row * stride;
            for (LONG column = 0; column < area.right - area.left; ++column, ++pixel)
            {
                if (!(pixel->rgbRed | pixel->rgbGreen | pixel->rgbBlue)) { pixel->rgbReserved = 0; continue; }
                const BYTE alpha = g_coverageAlpha[(pixel->rgbRed + 2 * pixel->rgbGreen + pixel->rgbBlue) / 4];
                pixel->rgbRed = static_cast<BYTE>(red * alpha / 255);
                pixel->rgbGreen = static_cast<BYTE>(green * alpha / 255);
                pixel->rgbBlue = static_cast<BYTE>(blue * alpha / 255);
                pixel->rgbReserved = alpha;
            }
        }
        return SUCCEEDED(EndBufferedPaint(paint, TRUE));
    }

    BOOL WINAPI Hook_ExtTextOutW(HDC hdc, int x, int y, UINT options, const RECT* rect, LPCWSTR text, UINT count, const INT* dx)
    {
        if (!IsTargetDC(hdc)) return Real_ExtTextOutW(hdc, x, y, options, rect, text, count, dx);
        const bool opaque = (options & ETO_OPAQUE) && rect && !IsRectEmpty(rect);
        const COLORREF background = GetBkColor(hdc);
        if (opaque) Count(L"ExtTextOut-opaque " + CallerModule(_ReturnAddress()) + L" bk " + ColorText(background));
        // Nothing to draw, or the current position moves with the text: leave those to GDI.
        const bool drawable = text && count && !(GetTextAlign(hdc) & TA_UPDATECP) &&
            !((options & ETO_CLIPPED) && (!rect || IsRectEmpty(rect))) && HasAlphaChannel(hdc);
        if (!drawable)
        {
            if (!opaque || !IsBackground(background)) return Real_ExtTextOutW(hdc, x, y, options, rect, text, count, dx);
            SetBkColor(hdc, RGB(0, 0, 0));
            const BOOL result = Real_ExtTextOutW(hdc, x, y, options, rect, text, count, dx);
            SetBkColor(hdc, background);
            return result;
        }
        // Opaque text fills its cell first: the app's background becomes glass, any other color a solid fill.
        if (opaque)
        {
            if (IsBackground(background) || background == RGB(0, 0, 0)) Real_FillRect(hdc, rect, g_black);
            else if (!FillOpaque(hdc, rect, background)) Real_ExtTextOutW(hdc, 0, 0, ETO_OPAQUE, rect, nullptr, 0, nullptr);
        }
        if (DrawAlphaText(hdc, x, y, options & ~ETO_OPAQUE, rect, text, count, dx)) return TRUE;
        Count(L"ExtTextOut-fallback " + CallerModule(_ReturnAddress()));
        return Real_ExtTextOutW(hdc, x, y, options & ~ETO_OPAQUE, rect, text, count, dx);
    }

    std::wstring SizeText(int width, int height)
    {
        // Exact for thin shapes (strips, borders), bucketed for big ones so the survey stays readable.
        auto bucket = [](int v) { return v <= 32 ? v : v / 100 * 100; };
        return std::to_wstring(bucket(width)) + L"x" + std::to_wstring(bucket(height));
    }

    std::wstring RectText(LPCRECT rect) { return rect ? SizeText(rect->right - rect->left, rect->bottom - rect->top) : L"?"; }

    std::wstring ThemeClass(HTHEME theme)
    {
        wchar_t name[128]{};
        if (!g_themeClassQuery || FAILED(g_themeClassQuery(theme, name, _countof(name)))) return L"?";
        return name;
    }

    // The dark ScrollBar theme paints arrows and thumbs as opaque dark tiles. Over glass they are redrawn as a glass
    // gutter with a translucent pill thumb and small arrow glyphs, both with real alpha (Direct2D, premultiplied).
    bool PaintScrollBar(HDC dc, int part, int state, LPCRECT rect)
    {
        const bool thumb = part == SBP_THUMBBTNHORZ || part == SBP_THUMBBTNVERT;
        const bool glass = part == SBP_LOWERTRACKHORZ || part == SBP_UPPERTRACKHORZ || part == SBP_LOWERTRACKVERT ||
            part == SBP_UPPERTRACKVERT || part == SBP_SIZEBOX || part == SBP_SIZEBOXBKGND;
        // The grip lines sit on top of a thumb that already says everything.
        if (part == SBP_GRIPPERHORZ || part == SBP_GRIPPERVERT) return true;
        if (!glass && !thumb && part != SBP_ARROWBTN) return false;
        if (!Real_FillRect(dc, rect, g_black)) return false;
        if (glass || !g_d2d || !HasAlphaChannel(dc)) return true;

        const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 0, 0, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
        Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> target;
        if (FAILED(g_d2d->CreateDCRenderTarget(&properties, &target)) || FAILED(target->BindDC(dc, rect))) return true;
        const float width = static_cast<float>(rect->right - rect->left), height = static_cast<float>(rect->bottom - rect->top);
        const float scale = GetDeviceCaps(dc, LOGPIXELSX) / 96.0f;
        target->BeginDraw();
        if (thumb)
        {
            // SCRBS_: 1 normal, 2 hot, 3 pressed, 4 disabled, 5 hover (pointer over the bar).
            const bool active = state == SCRBS_HOT || state == SCRBS_PRESSED;
            const bool vertical = part == SBP_THUMBBTNVERT;
            const float thickness = (vertical ? width : height) * (active || state == SCRBS_HOVER ? 0.45f : 0.3f);
            const float inset = 1.5f * scale;
            const auto bounds = vertical
                ? D2D1::RectF((width - thickness) / 2, inset, (width + thickness) / 2, height - inset)
                : D2D1::RectF(inset, (height - thickness) / 2, width - inset, (height + thickness) / 2);
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
            target->CreateSolidColorBrush(active ? D2D1::ColorF(0.88f, 0.88f, 0.88f, 0.65f) : D2D1::ColorF(0.63f, 0.63f, 0.63f, 0.5f), &brush);
            if (brush) target->FillRoundedRectangle(D2D1::RoundedRect(bounds, thickness / 2, thickness / 2), brush.Get());
        }
        else
        {
            // ABS_: four directions of four states (normal, hot, pressed, disabled), then four hover states.
            const int direction = state >= ABS_UPHOVER ? state - ABS_UPHOVER : (state - 1) / 4;
            const int look = state >= ABS_UPHOVER ? 0 : (state - 1) % 4;
            const float half = (look == 1 || look == 2 ? 4.5f : 3.5f) * scale, depth = half * 0.65f;
            const float cx = width / 2, cy = height / 2;
            D2D1_POINT_2F tip{}, left{}, right{};
            switch (direction)
            {
            case 0: tip = { cx, cy - depth }; left = { cx - half, cy + depth }; right = { cx + half, cy + depth }; break;
            case 1: tip = { cx, cy + depth }; left = { cx + half, cy - depth }; right = { cx - half, cy - depth }; break;
            case 2: tip = { cx - depth, cy }; left = { cx + depth, cy + half }; right = { cx + depth, cy - half }; break;
            default: tip = { cx + depth, cy }; left = { cx - depth, cy - half }; right = { cx - depth, cy + half }; break;
            }
            Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
            const auto color = look == 3 ? D2D1::ColorF(0.5f, 0.5f, 0.5f, 0.3f)
                : look == 0 ? D2D1::ColorF(0.63f, 0.63f, 0.63f, 0.55f) : D2D1::ColorF(0.88f, 0.88f, 0.88f, 0.8f);
            if (SUCCEEDED(g_d2d->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink)))
            {
                sink->BeginFigure(tip, D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(left);
                sink->AddLine(right);
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                target->CreateSolidColorBrush(color, &brush);
                if (brush) target->FillGeometry(path.Get(), brush.Get());
            }
        }
        target->EndDraw();
        return true;
    }

    // The dark theme's hover/selection artwork is an opaque grey tile. Over glass it is replaced by a translucent
    // rounded highlight, blended onto whatever is already there; selected rows also get an accent pill on the left.
    bool PaintHighlight(HDC dc, LPCRECT rect, float alpha, bool pill)
    {
        if (!g_d2d || !rect || IsRectEmpty(rect) || !HasAlphaChannel(dc)) return false;
        BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        BP_PAINTPARAMS params{ sizeof params, BPPF_ERASE, nullptr, &blend };
        HDC buffer = nullptr;
        const HPAINTBUFFER paint = BeginBufferedPaint(dc, rect, BPBF_TOPDOWNDIB, &params, &buffer);
        if (!paint) return false;
        const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 0, 0, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
        Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> target;
        if (FAILED(g_d2d->CreateDCRenderTarget(&properties, &target)) || FAILED(target->BindDC(buffer, rect)))
        {
            EndBufferedPaint(paint, FALSE);
            return false;
        }
        const float width = static_cast<float>(rect->right - rect->left), height = static_cast<float>(rect->bottom - rect->top);
        const float scale = GetDeviceCaps(dc, LOGPIXELSX) / 96.0f, radius = 4 * scale;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        target->BeginDraw();
        target->CreateSolidColorBrush(D2D1::ColorF(0.56f, 0.56f, 0.56f, alpha), &brush);
        if (brush) target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, width - 0.5f, height - 0.5f), radius, radius), brush.Get());
        if (brush && pill && height > 12 * scale)
        {
            DWORD accent = 0;
            BOOL opaque = FALSE;
            const auto color = SUCCEEDED(DwmGetColorizationColor(&accent, &opaque))
                ? D2D1::ColorF(((accent >> 16) & 0xFF) / 255.0f, ((accent >> 8) & 0xFF) / 255.0f, (accent & 0xFF) / 255.0f)
                : D2D1::ColorF(0.38f, 0.8f, 1.0f);
            brush->SetColor(color);
            const float inset = height * 0.28f, pillWidth = 3 * scale;
            target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(1, inset, 1 + pillWidth, height - inset), pillWidth / 2, pillWidth / 2), brush.Get());
        }
        const bool drawn = SUCCEEDED(target->EndDraw());
        EndBufferedPaint(paint, drawn);
        return drawn;
    }

    bool PaintGlassTheme(HTHEME theme, HDC dc, int part, int state, LPCRECT rect, LPCRECT clip)
    {
        if (!rect || !IsTargetDC(dc)) return false;
        const auto name = ThemeClass(theme);
        if (_wcsicmp(name.c_str(), L"ScrollBar") == 0)
        {
            const int saved = SaveDC(dc);
            if (!saved) return false;
            bool painted = false;
            if (!clip || IntersectClipRect(dc, clip->left, clip->top, clip->right, clip->bottom) != ERROR)
                painted = PaintScrollBar(dc, part, state, rect);
            RestoreDC(dc, saved);
            if (painted) Count(L"GlassScrollBar part " + std::to_wstring(part) + L" state " + std::to_wstring(state));
            return painted;
        }
        // File list and drive tiles (ListView, painted by DirectUI) and the navigation tree share state numbers:
        // 1 normal, 2 hot, 3 selected, 4 disabled, 5 selected without focus, 6 hot and selected.
        const bool list = _wcsicmp(name.c_str(), L"ListView") == 0;
        const bool tree = _wcsicmp(name.c_str(), L"TreeView") == 0;
        const bool item = (list && part == LVP_LISTITEM) || (tree && part == TVP_TREEITEM);
        // Group headers ("Devices and drives") have no artwork while plain open (1) or collapsed (9); every hot,
        // selected or mixed state gets a light highlight.
        const bool groupHover = list && part == LVP_GROUPHEADER && state != LVGH_OPEN && state != LVGH_CLOSE;
        if ((item && state != 1 && state != 4) || groupHover)
        {
            const float alpha = groupHover ? 0.14f : state == 2 ? 0.18f : state == 5 ? 0.16f : state == 6 ? 0.34f : 0.28f;
            const int saved = SaveDC(dc);
            if (!saved) return false;
            bool painted = false;
            if (!clip || IntersectClipRect(dc, clip->left, clip->top, clip->right, clip->bottom) != ERROR)
                painted = PaintHighlight(dc, rect, alpha, tree && state != 2);
            RestoreDC(dc, saved);
            if (painted) Count(L"GlassHighlight " + name + L" part " + std::to_wstring(part) + L" state " + std::to_wstring(state));
            return painted;
        }
        // Part IDs are meaningful only together with the theme class. Never
        // erase header sort arrows or selection artwork.
        const bool header = _wcsicmp(name.c_str(), L"Header") == 0;
        const bool separator = _wcsicmp(name.c_str(), L"PreviewPane") == 0 && (part == 3 || part == 4);
        const bool normalHeader = header && (part == 0 || (part == HP_HEADERITEM &&
            (state == HIS_NORMAL || state == HIS_SORTEDNORMAL || state == HIS_ICONNORMAL || state == HIS_ICONSORTEDNORMAL)));
        if (!normalHeader && !separator) return false;
        const int saved = SaveDC(dc);
        if (!saved) return false;
        if (clip && IntersectClipRect(dc, clip->left, clip->top, clip->right, clip->bottom) == ERROR)
        {
            RestoreDC(dc, saved);
            return false;
        }
        const bool painted = Real_FillRect(dc, rect, g_black) != 0;
        RestoreDC(dc, saved);
        if (painted) Count(L"GlassTheme " + name + L" part " + std::to_wstring(part));
        return painted;
    }

    HRESULT WINAPI Hook_DrawThemeBackground(HTHEME theme, HDC hdc, int part, int state, LPCRECT rect, LPCRECT clip)
    {
        if (!IsTargetDC(hdc)) return Real_DrawThemeBackground(theme, hdc, part, state, rect, clip);
        if (PaintGlassTheme(theme, hdc, part, state, rect, clip)) return S_OK;
        Count(L"DrawThemeBackground " + ThemeClass(theme) + L" " + CallerModule(_ReturnAddress()) + L" part " + std::to_wstring(part) + L" state " + std::to_wstring(state) + L" " + RectText(rect));
        return Real_DrawThemeBackground(theme, hdc, part, state, rect, clip);
    }

    HRESULT WINAPI Hook_DrawThemeBackgroundEx(HTHEME theme, HDC hdc, int part, int state, LPCRECT rect, const DTBGOPTS* options)
    {
        if (!IsTargetDC(hdc)) return Real_DrawThemeBackgroundEx(theme, hdc, part, state, rect, options);
        const bool supported = !options || (options->dwSize == sizeof(DTBGOPTS) && (options->dwFlags & ~DTBG_CLIPRECT) == 0);
        const RECT* clip = options && (options->dwFlags & DTBG_CLIPRECT) ? &options->rcClip : nullptr;
        if (supported && PaintGlassTheme(theme, hdc, part, state, rect, clip)) return S_OK;
        Count(L"DrawThemeBackgroundEx " + ThemeClass(theme) + L" " + CallerModule(_ReturnAddress()) + L" part " + std::to_wstring(part) + L" state " + std::to_wstring(state) + L" " + RectText(rect));
        return Real_DrawThemeBackgroundEx(theme, hdc, part, state, rect, options);
    }

    BOOL WINAPI Hook_BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop)
    {
        // Only thin blits (strips, borders, scrollbar parts) are what we are hunting here.
        if (IsTargetDC(dst) && (w <= 32 || h <= 32)) Count(L"BitBlt " + CallerModule(_ReturnAddress()) + L" " + SizeText(w, h) + L" rop " + std::to_wstring(rop));
        return Real_BitBlt(dst, x, y, w, h, src, sx, sy, rop);
    }

    BOOL WINAPI Hook_GdiAlphaBlend(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, BLENDFUNCTION blend)
    {
        if (IsTargetDC(dst)) Count(L"AlphaBlend " + CallerModule(_ReturnAddress()) + L" " + SizeText(w, h) + L" fmt " + std::to_wstring(blend.AlphaFormat));
        return Real_GdiAlphaBlend(dst, x, y, w, h, src, sx, sy, sw, sh, blend);
    }

    template <typename T> LONG Attach(T*& real, T* hook) { return DetourAttach(reinterpret_cast<PVOID*>(&real), reinterpret_cast<PVOID>(hook)); }
    template <typename T> LONG Detach(T*& real, T* hook) { return DetourDetach(reinterpret_cast<PVOID*>(&real), reinterpret_cast<PVOID>(hook)); }

    // Runs on the window's own thread: Explorer gives every folder window a thread of its own.
    void ApplyGlass(HWND root)
    {
        int original = 0;
        DwmGetWindowAttribute(root, DWMWA_SYSTEMBACKDROP_TYPE, &original, sizeof original);
        AcquireSRWLockExclusive(&g_rootLock);
        g_roots[root] = original;
        ReleaseSRWLockExclusive(&g_rootLock);
        t_root = root;
        Real_DwmSetWindowAttribute(root, DWMWA_SYSTEMBACKDROP_TYPE, &Acrylic, sizeof Acrylic);
        Real_DwmExtendFrameIntoClientArea(root, &WholeWindow);
        SetWindowSubclass(root, RootSubclass, SubclassId, 0);
        DefSubclassProc(root, WM_NCACTIVATE, TRUE, 0);
        RedrawWindow(root, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
        Log(L"glass applied to %p on thread %lu (backdrop was %d)", root, GetCurrentThreadId(), original);
        AddXamlThread(g_module, GetCurrentThreadId());
        SetTimer(root, XamlTimerId, 1000, nullptr);
    }

    // Also runs on the window's thread; returns whether any styled window is left in the process. A window being
    // destroyed only needs forgetting, not repainting.
    bool RemoveGlass(bool destroying)
    {
        const HWND root = std::exchange(t_root, nullptr);
        if (root)
        {
            RemoveXamlThread();
            AcquireSRWLockExclusive(&g_rootLock);
            const auto entry = g_roots.find(root);
            int original = entry != g_roots.end() ? entry->second : 0;
            if (entry != g_roots.end()) g_roots.erase(entry);
            ReleaseSRWLockExclusive(&g_rootLock);
            if (IsWindow(root))
            {
                KillTimer(root, XamlTimerId);
                RemoveWindowSubclass(root, RootSubclass, SubclassId);
            }
            if (!destroying && IsWindow(root))
            {
                // Give back the backdrop Explorer last asked for, and let it redo its own frame margins.
                Real_DwmSetWindowAttribute(root, DWMWA_SYSTEMBACKDROP_TYPE, &original, sizeof original);
                constexpr MARGINS none{};
                Real_DwmExtendFrameIntoClientArea(root, &none);
                SendMessageW(root, WM_DWMCOMPOSITIONCHANGED, 0, 0);
                RedrawWindow(root, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
            }
            Log(L"glass removed from %p", root);
        }
        AcquireSRWLockShared(&g_rootLock);
        const bool others = !g_roots.empty();
        ReleaseSRWLockShared(&g_rootLock);
        return others;
    }

    bool SurveyRequested()
    {
        wchar_t path[MAX_PATH]{};
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH)) return false;
        wcscat_s(path, L"\\GlassForge\\hook.survey");
        return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    }

    void Install()
    {
        AcquireSRWLockExclusive(&g_installLock);
        if (!g_installed)
        {
            // Undocumented UxTheme class query: optional, fail closed if absent.
            g_themeClassQuery = reinterpret_cast<ThemeClassQuery>(GetProcAddress(GetModuleHandleW(L"uxtheme.dll"), MAKEINTRESOURCEA(74)));
            Log(L"theme class query %s", g_themeClassQuery ? L"available" : L"unavailable; themed parts unchanged");
            g_survey = SurveyRequested();
            BuildCoverageAlpha();
            BufferedPaintInit();
            const HRESULT d2d = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, g_d2d.ReleaseAndGetAddressOf());
            Log(L"direct2d factory %08X, survey %s", d2d, g_survey ? L"on" : L"off");
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            Attach(Real_FillRect, Hook_FillRect);
            Attach(Real_CreateCompatibleDC, Hook_CreateCompatibleDC);
            Attach(Real_DeleteDC, Hook_DeleteDC);
            Attach(Real_PatBlt, Hook_PatBlt);
            Attach(Real_ExtTextOutW, Hook_ExtTextOutW);
            Attach(Real_DrawThemeBackground, Hook_DrawThemeBackground);
            Attach(Real_DrawThemeBackgroundEx, Hook_DrawThemeBackgroundEx);
            if (g_survey)
            {
                Attach(Real_BitBlt, Hook_BitBlt);
                Attach(Real_GdiAlphaBlend, Hook_GdiAlphaBlend);
            }
            Attach(Real_DwmSetWindowAttribute, Hook_DwmSetWindowAttribute);
            Attach(Real_DwmExtendFrameIntoClientArea, Hook_DwmExtendFrameIntoClientArea);
            const LONG result = DetourTransactionCommit();
            g_installed = result == NO_ERROR;
            Log(L"detours %s (%ld)", g_installed ? L"installed" : L"FAILED", result);
        }
        ReleaseSRWLockExclusive(&g_installLock);
    }

    void Uninstall()
    {
        AcquireSRWLockExclusive(&g_installLock);
        if (g_installed)
        {
            StopXamlStyling();
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            Detach(Real_FillRect, Hook_FillRect);
            Detach(Real_CreateCompatibleDC, Hook_CreateCompatibleDC);
            Detach(Real_DeleteDC, Hook_DeleteDC);
            Detach(Real_PatBlt, Hook_PatBlt);
            Detach(Real_ExtTextOutW, Hook_ExtTextOutW);
            Detach(Real_DrawThemeBackground, Hook_DrawThemeBackground);
            Detach(Real_DrawThemeBackgroundEx, Hook_DrawThemeBackgroundEx);
            if (g_survey)
            {
                Detach(Real_BitBlt, Hook_BitBlt);
                Detach(Real_GdiAlphaBlend, Hook_GdiAlphaBlend);
            }
            Detach(Real_DwmSetWindowAttribute, Hook_DwmSetWindowAttribute);
            Detach(Real_DwmExtendFrameIntoClientArea, Hook_DwmExtendFrameIntoClientArea);
            g_installed = DetourTransactionCommit() != NO_ERROR;
            Log(L"detours removed");
        }
        ReleaseSRWLockExclusive(&g_installLock);
    }

    void Dump()
    {
        AcquireSRWLockExclusive(&g_statsLock);
        Log(L"--- survey: %zu distinct calls ---", g_stats.size());
        for (const auto& [key, count] : g_stats) Log(L"%6u  %s", count, key.c_str());
        g_stats.clear();
        ReleaseSRWLockExclusive(&g_statsLock);
    }
}

// Installed per Explorer window thread with SetWindowsHookEx(WH_GETMESSAGE). Registered messages: Attach re-enables
// a thread after an earlier Detach; Detach restores the thread's window (and removes the detours once no styled
// window is left in the process); Dump writes the survey.
extern "C" __declspec(dllexport) LRESULT CALLBACK GlassForgeGetMessageProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && wParam == PM_REMOVE)
    {
        if (!t_seen)
        {
            t_seen = true;
            g_dumpMessage = RegisterWindowMessageW(L"GlassForge.Hook.Dump");
            g_detachMessage = RegisterWindowMessageW(L"GlassForge.Hook.Detach");
            g_attachMessage = RegisterWindowMessageW(L"GlassForge.Hook.Attach");
            Log(L"hook active on thread %lu", GetCurrentThreadId());
        }
        const auto* message = reinterpret_cast<const MSG*>(lParam);
        if (message->message == g_attachMessage) t_detached = false;
        if (!t_root && !t_detached && message->hwnd)
        {
            const HWND root = GetAncestor(message->hwnd, GA_ROOT);
            wchar_t name[64]{};
            GetClassNameW(root, name, _countof(name));
            if (_wcsicmp(name, L"CabinetWClass") == 0)
            {
                Install();
                if (g_installed) ApplyGlass(root);
            }
        }
        if (message->message == g_dumpMessage) Dump();
        else if (message->message == g_detachMessage)
        {
            t_detached = true;
            if (!RemoveGlass()) Uninstall();
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        g_module = module;
        DisableThreadLibraryCalls(module);
        Log(L"loaded");
        break;
    case DLL_PROCESS_DETACH:
        // Never leave detours pointing into an unloaded DLL; GlassForge normally sends Detach first.
        Uninstall();
        Log(L"unloaded");
        break;
    }
    return TRUE;
}
