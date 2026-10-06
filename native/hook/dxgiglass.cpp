// Experimental glass for apps that draw with DXGI (Chromium/Electron, Qt Quick/RHI, Flutter, most modern apps).
//
// These apps paint their own solid background color into a swap chain, so no backdrop brush can be swapped (the
// Office/Settings route) and no GDI fill can be redrawn (the Explorer route). Instead, every frame, just before the
// app presents it, the frame goes through "color-to-alpha": each pixel is split into the app's background color and
// the least-transparent foreground that, laid over that color, gives the same pixel. The background part becomes
// transparency, so DWM's backdrop shows through, and anti-aliased text keeps clean edges because only exactly the
// background's share of each pixel is removed. Output is premultiplied, which DWM reads correctly with HDR on too.
//
// Present and Present1 are detoured once for the whole process (their addresses come from a throwaway swap chain),
// so swap chains created before the hook arrived are covered. The pass runs in its own ID3DDeviceContextState, so
// none of the app's pipeline state is touched.
#include "dxgiglass.h"
#include <dxgi1_2.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <cstdlib>
#include <algorithm>
#include <vector>
#include <wrl/client.h>
#include <map>
#include <string>
#include "detours.h"
#include "log.h"

using Microsoft::WRL::ComPtr;

namespace
{
    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    using CreateForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
    using CreateLegacyFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
    using CreateForCompositionFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
    PresentFn Real_Present = nullptr;
    Present1Fn Real_Present1 = nullptr;
    CreateForHwndFn Real_CreateSwapChainForHwnd = nullptr;
    CreateLegacyFn Real_CreateSwapChain = nullptr;
    CreateForCompositionFn Real_CreateSwapChainForComposition = nullptr;

    // A window-bound swap chain's alpha is ignored by DWM, so the frame can't show the backdrop. Swap chains the app
    // creates for a window are made for composition instead and shown on the same window through DirectComposition,
    // which honors premultiplied alpha. These keep the composition trees alive for the life of the process.
    struct WindowVisual
    {
        ComPtr<IDCompositionDesktopDevice> device;
        ComPtr<IDCompositionTarget> target;
        ComPtr<IDCompositionVisual2> visual;
    };
    std::vector<WindowVisual> g_visuals;
    // The window each substituted swap chain draws into, to skip the glass while it is full screen.
    std::map<IDXGISwapChain*, HWND> g_chainWindows;

    // Full screen (a window covering its whole monitor: a remote session, video, a game) gets no glass at all.
    bool IsFullscreen(HWND window)
    {
        RECT rect{};
        MONITORINFO monitor{ sizeof monitor };
        const HWND root = GetAncestor(window, GA_ROOT);
        return GetWindowRect(root, &rect) && GetMonitorInfoW(MonitorFromWindow(root, MONITOR_DEFAULTTONEAREST), &monitor)
            && rect.left <= monitor.rcMonitor.left && rect.top <= monitor.rcMonitor.top && rect.right >= monitor.rcMonitor.right
            && rect.bottom >= monitor.rcMonitor.bottom;
    }
    bool g_installed = false;
    SRWLOCK g_lock = SRWLOCK_INIT;
    SRWLOCK g_windowLock = SRWLOCK_INIT; // g_originalProcs only, taken on every window message

    // How much of the background stays: 0 = fully clear, 1 = untouched. A little keeps text readable on busy wallpaper.
    constexpr float BackgroundFloor = 0.12f;
    // When to look for the background color: a few times early (apps show a splash screen in a different color first),
    // then every 600 frames in case the app switches theme.
    bool DetectNow(unsigned frame) { return frame == 0 || frame == 30 || frame == 120 || frame == 300 || frame % 1800 == 0; }

    constexpr char ShaderSource[] = R"(
cbuffer Params : register(b0) { float3 Background; float Floor; uint KeepCount; uint ClearCount; uint2 Padding; int4 Keep[8]; int4 Clear[8]; float4 Background2; };
Texture2D<float4> Frame : register(t0);
float4 VS(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PS(float4 position : SV_Position) : SV_Target
{
    float3 c = Frame.Load(int3(position.xy, 0)).rgb;
    // Areas left exactly as the app drew them (a remote desktop view, a video), fully solid.
    for (uint i = 0; i < KeepCount; ++i)
        if (position.x >= Keep[i].x && position.y >= Keep[i].y && position.x < Keep[i].z && position.y < Keep[i].w) return float4(c, 1);
    // What trimming cut off those areas (letterbox bars, which are darker than the background and so can't be
    // color-to-alpha'd away) shows as plain glass with the usual tint.
    for (uint j = 0; j < ClearCount; ++j)
        if (position.x >= Clear[j].x && position.y >= Clear[j].y && position.x < Clear[j].z && position.y < Clear[j].w) return float4(Background * Floor, Floor);
    float3 b = Background;
    // Apps often have a second large flat color (a black title strip or sidebar). Where a pixel's surroundings are
    // mostly that color, it is the key instead, so text on it separates cleanly too.
    if (Background2.a > 0)
    {
        int votes = 0;
        const int2 offsets[5] = { int2(0, 0), int2(-6, 0), int2(6, 0), int2(0, -6), int2(0, 6) };
        for (int k = 0; k < 5; ++k)
        {
            float3 n = Frame.Load(int3(int2(position.xy) + offsets[k], 0)).rgb;
            float d1 = max(max(abs(n.r - Background.r), abs(n.g - Background.g)), abs(n.b - Background.b));
            float d2 = max(max(abs(n.r - Background2.r), abs(n.g - Background2.g)), abs(n.b - Background2.b));
            votes += d2 < 0.01 ? 1 : (d1 < 0.01 ? -1 : 0);
        }
        if (votes > 0) b = Background2.rgb;
    }
    // Per channel, how far the pixel is from the background toward white or black, as a share of the room there is.
    float3 up = (c - b) / max(1 - b, 1e-4);
    float3 down = (b - c) / max(b, 1e-4);
    float3 need = max(max(up, down), 0);
    float alpha = saturate(max(max(need.r, need.g), max(need.b, Floor)));
    // Premultiplied foreground: the pixel minus the background's remaining share.
    float3 rgb = clamp(c - (1 - alpha) * b, 0, alpha);
    return float4(rgb, alpha);
}
)";

    ComPtr<ID3DBlob> g_vsCode, g_psCode;

    // Rectangles to keep solid, in the window's client pixels, published by GlassForge from outside the process (it
    // finds them with UI Automation) in Local\GlassForge.DxgiKeep.<pid>. Layout: UINT32 count, then up to 8 RECTs.
    struct KeepAreas
    {
        UINT32 count;
        RECT rects[8];
    };
    struct Params
    {
        float background[3];
        float floor;
        UINT32 keepCount;
        UINT32 clearCount;
        UINT32 padding[2];
        INT32 keep[8][4];
        INT32 clear[8][4];
        float background2[4]; // rgb, a = 1 when present
    };
    static_assert(sizeof(Params) == 304);
    HANDLE g_keepMapping = nullptr;
    const KeepAreas* g_keepView = nullptr;
    unsigned g_keepRetry = 0;

    void ReadKeepAreas(Params& params)
    {
        if (!g_keepView && g_keepRetry++ % 60 == 0)
        {
            wchar_t name[64]{};
            swprintf_s(name, L"Local\\GlassForge.DxgiKeep.%lu", GetCurrentProcessId());
            if ((g_keepMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name)) != nullptr)
            {
                g_keepView = static_cast<const KeepAreas*>(MapViewOfFile(g_keepMapping, FILE_MAP_READ, 0, 0, sizeof(KeepAreas)));
                Log(L"dxgi: keep-solid areas %s", g_keepView ? L"connected" : L"could not be mapped");
            }
        }
        if (!g_keepView) return;
        const KeepAreas areas = *g_keepView;
        params.keepCount = std::min<UINT32>(areas.count, 8);
        for (UINT32 i = 0; i < params.keepCount; ++i)
        {
            params.keep[i][0] = areas.rects[i].left;
            params.keep[i][1] = areas.rects[i].top;
            params.keep[i][2] = areas.rects[i].right;
            params.keep[i][3] = areas.rects[i].bottom;
        }
    }

    struct DeviceResources
    {
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        ComPtr<ID3D11Buffer> params;
        ComPtr<ID3D11BlendState> noBlend;
        ComPtr<ID3DDeviceContextState> state;
        bool ok = false;
    };

    struct ChainResources
    {
        ComPtr<ID3D11Texture2D> copy;
        ComPtr<ID3D11ShaderResourceView> copyView;
        UINT width = 0, height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        float background[3]{};
        float background2[3]{};
        bool haveBackground = false;
        bool haveBackground2 = false;
        unsigned frames = 0;
        bool logged = false;
        bool unsupported = false;
        ComPtr<ID3D11Texture2D> detectStaging;
        bool detectPending = false;
        // Letterbox trimming of the keep-solid areas: a video shown "contained" (LogMeIn's remote screen) has black
        // bars that belong to the same element. A copy of the areas is read back without stalling, and each area
        // shrinks to its rows and columns that aren't pure black.
        ComPtr<ID3D11Texture2D> trimStaging;
        RECT trimSource[8]{};
        RECT trimmed[8]{};
        UINT32 trimCount = 0;
        RECT pendingSource[8]{};
        UINT32 pendingCount = 0;
        bool trimPending = false;
        unsigned trimAge = 0;
    };

    std::map<ID3D11Device*, DeviceResources> g_devices;
    std::map<IDXGISwapChain*, ChainResources> g_chains;

    bool SameRects(const RECT* a, const RECT* b, UINT32 count)
    {
        for (UINT32 i = 0; i < count; ++i)
            if (!EqualRect(&a[i], &b[i])) return false;
        return true;
    }

    // Shrinks one area to the bounding box of its pixels that aren't (near) pure black; empty when it is all black.
    RECT TrimBlack(const D3D11_MAPPED_SUBRESOURCE& mapped, RECT area)
    {
        constexpr BYTE Black = 6;
        auto lit = [&](LONG x, LONG y) {
            const BYTE* pixel = static_cast<const BYTE*>(mapped.pData) + y * mapped.RowPitch + x * 4;
            return pixel[0] > Black || pixel[1] > Black || pixel[2] > Black;
        };
        // A row or column counts as picture when at least 1% of it is lit, ignoring a few pixels at each end: the
        // element's own edges can carry a 1 px border or a neighbor's divider, which would otherwise make every row count.
        constexpr LONG Edge = 4;
        auto rowLit = [&](LONG y) {
            LONG count = 0;
            for (LONG x = area.left + Edge; x < area.right - Edge; x += 2) count += lit(x, y);
            return count > std::max<LONG>(2, (area.right - area.left) / 200);
        };
        auto columnLit = [&](LONG x, LONG top, LONG bottom) {
            LONG count = 0;
            for (LONG y = top + Edge; y < bottom - Edge; y += 2) count += lit(x, y);
            return count > std::max<LONG>(2, (bottom - top) / 200);
        };
        RECT result = area;
        while (result.top < result.bottom && !rowLit(result.top)) ++result.top;
        while (result.bottom > result.top && !rowLit(result.bottom - 1)) --result.bottom;
        if (result.top >= result.bottom) return RECT{};
        while (result.left < result.right && !columnLit(result.left, result.top, result.bottom)) ++result.left;
        while (result.right > result.left && !columnLit(result.right - 1, result.top, result.bottom)) --result.right;
        return result;
    }

    // Starts a read-back when the areas changed (or every ~half second), finishes it once the GPU is done, and swaps
    // the published areas for their trimmed versions when those were measured for exactly these areas.
    void TrimKeepAreas(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* frame, ChainResources& chain, Params& params)
    {
        if (params.keepCount == 0) return;
        RECT areas[8]{};
        D3D11_TEXTURE2D_DESC desc{};
        frame->GetDesc(&desc);
        for (UINT32 i = 0; i < params.keepCount; ++i)
        {
            areas[i] = { std::clamp<LONG>(params.keep[i][0], 0, desc.Width), std::clamp<LONG>(params.keep[i][1], 0, desc.Height),
                std::clamp<LONG>(params.keep[i][2], 0, desc.Width), std::clamp<LONG>(params.keep[i][3], 0, desc.Height) };
        }
        if (chain.trimPending)
        {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (context->Map(chain.trimStaging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) == S_OK)
            {
                for (UINT32 i = 0; i < chain.pendingCount; ++i)
                {
                    const RECT before = chain.trimmed[i];
                    chain.trimmed[i] = TrimBlack(mapped, chain.pendingSource[i]);
                    if (!EqualRect(&before, &chain.trimmed[i]))
                    {
                        const RECT& a = chain.pendingSource[i];
                        const RECT& t = chain.trimmed[i];
                        const BYTE* corner = static_cast<const BYTE*>(mapped.pData) + (a.top + 2) * mapped.RowPitch + (a.left + 2) * 4;
                        Log(L"dxgi: keep area %ld,%ld-%ld,%ld trimmed to %ld,%ld-%ld,%ld (frame %ux%u, pixel near corner %02X%02X%02X)", a.left, a.top,
                            a.right, a.bottom, t.left, t.top, t.right, t.bottom, desc.Width, desc.Height, corner[0], corner[1], corner[2]);
                    }
                }
                context->Unmap(chain.trimStaging.Get(), 0);
                std::copy_n(chain.pendingSource, chain.pendingCount, chain.trimSource);
                chain.trimCount = chain.pendingCount;
                chain.trimPending = false;
            }
        }
        const bool current = chain.trimCount == params.keepCount && SameRects(areas, chain.trimSource, params.keepCount);
        if (!chain.trimPending && (!current || ++chain.trimAge >= 60))
        {
            chain.trimAge = 0;
            D3D11_TEXTURE2D_DESC stagingDesc{};
            if (chain.trimStaging) chain.trimStaging->GetDesc(&stagingDesc);
            if (!chain.trimStaging || stagingDesc.Width != desc.Width || stagingDesc.Height != desc.Height || stagingDesc.Format != desc.Format)
            {
                stagingDesc = desc;
                stagingDesc.BindFlags = 0;
                stagingDesc.MiscFlags = 0;
                stagingDesc.Usage = D3D11_USAGE_STAGING;
                stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                chain.trimStaging.Reset();
                if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &chain.trimStaging))) return;
            }
            for (UINT32 i = 0; i < params.keepCount; ++i)
            {
                if (IsRectEmpty(&areas[i])) continue;
                const D3D11_BOX box{ UINT(areas[i].left), UINT(areas[i].top), 0, UINT(areas[i].right), UINT(areas[i].bottom), 1 };
                context->CopySubresourceRegion(chain.trimStaging.Get(), 0, box.left, box.top, 0, chain.copy.Get(), 0, &box);
            }
            std::copy_n(areas, params.keepCount, chain.pendingSource);
            chain.pendingCount = params.keepCount;
            chain.trimPending = true;
        }
        RECT trimmed[8]{};
        std::copy_n(chain.trimmed, 8, trimmed);
        if (!current)
        {
            // Until measured, an area the same size as a measured one (it moved, or the reading jittered) borrows that
            // measurement, shifted; anything else stays fully solid.
            if (chain.trimCount != params.keepCount) return;
            for (UINT32 i = 0; i < params.keepCount; ++i)
            {
                const RECT& was = chain.trimSource[i];
                if (std::abs((areas[i].right - areas[i].left) - (was.right - was.left)) > 4
                    || std::abs((areas[i].bottom - areas[i].top) - (was.bottom - was.top)) > 4)
                    return;
                OffsetRect(&trimmed[i], areas[i].left - was.left, areas[i].top - was.top);
                IntersectRect(&trimmed[i], &trimmed[i], &areas[i]);
            }
        }
        for (UINT32 i = 0; i < params.keepCount; ++i)
        {
            params.clear[i][0] = areas[i].left;
            params.clear[i][1] = areas[i].top;
            params.clear[i][2] = areas[i].right;
            params.clear[i][3] = areas[i].bottom;
        }
        params.clearCount = params.keepCount;
        UINT32 kept = 0;
        for (UINT32 i = 0; i < params.keepCount; ++i)
        {
            const RECT& r = trimmed[i];
            if (IsRectEmpty(&r)) continue; // all black: nothing worth keeping solid
            params.keep[kept][0] = r.left;
            params.keep[kept][1] = r.top;
            params.keep[kept][2] = r.right;
            params.keep[kept][3] = r.bottom;
            ++kept;
        }
        params.keepCount = kept;
    }

    bool CompileShaders()
    {
        if (g_vsCode && g_psCode) return true;
        ComPtr<ID3DBlob> errors;
        if (FAILED(D3DCompile(ShaderSource, sizeof ShaderSource - 1, "glass", nullptr, nullptr, "VS", "vs_4_0", 0, 0, &g_vsCode, &errors)) ||
            FAILED(D3DCompile(ShaderSource, sizeof ShaderSource - 1, "glass", nullptr, nullptr, "PS", "ps_4_0", 0, 0, &g_psCode, &errors)))
        {
            Log(L"dxgi: shader compile failed: %S", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
            return false;
        }
        return true;
    }

    DeviceResources& ResourcesFor(ID3D11Device* device)
    {
        auto [it, added] = g_devices.try_emplace(device);
        auto& r = it->second;
        if (!added) return r;
        ComPtr<ID3D11Device1> device1;
        if (!CompileShaders() || FAILED(device->QueryInterface(IID_PPV_ARGS(&device1)))) return r;
        const D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
        const UINT flags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
        D3D11_BUFFER_DESC params{ sizeof(Params), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        r.ok = SUCCEEDED(device1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &r.state))
            && SUCCEEDED(device->CreateVertexShader(g_vsCode->GetBufferPointer(), g_vsCode->GetBufferSize(), nullptr, &r.vs))
            && SUCCEEDED(device->CreatePixelShader(g_psCode->GetBufferPointer(), g_psCode->GetBufferSize(), nullptr, &r.ps))
            && SUCCEEDED(device->CreateBuffer(&params, nullptr, &r.params))
            && SUCCEEDED(device->CreateBlendState(&blend, &r.noBlend));
        Log(L"dxgi: device %p (feature level %X) resources %s", device, level, r.ok ? L"ready" : L"FAILED");
        return r;
    }

    bool Is8Bit(DXGI_FORMAT format)
    {
        return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    // The background is the most common color on a coarse grid of the untouched frame. Never stalls the app: a copy is
    // started when asked (start), and read on a later frame once the GPU has finished it.
    void DetectBackground(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* frame, ChainResources& chain, bool start)
    {
        D3D11_TEXTURE2D_DESC desc{};
        frame->GetDesc(&desc);
        if (start && !chain.detectPending)
        {
            D3D11_TEXTURE2D_DESC have{};
            if (chain.detectStaging) chain.detectStaging->GetDesc(&have);
            if (!chain.detectStaging || have.Width != desc.Width || have.Height != desc.Height || have.Format != desc.Format)
            {
                D3D11_TEXTURE2D_DESC stagingDesc = desc;
                stagingDesc.BindFlags = 0;
                stagingDesc.MiscFlags = 0;
                stagingDesc.Usage = D3D11_USAGE_STAGING;
                stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                chain.detectStaging.Reset();
                if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &chain.detectStaging))) return;
            }
            context->CopyResource(chain.detectStaging.Get(), frame);
            chain.detectPending = true;
            return;
        }
        if (!chain.detectPending) return;
        chain.detectStaging->GetDesc(&desc);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (context->Map(chain.detectStaging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) != S_OK) return;
        chain.detectPending = false;
        std::map<UINT32, unsigned> counts;
        unsigned samples = 0;
        for (UINT y = desc.Height / 20; y < desc.Height; y += desc.Height / 20 + 1)
            for (UINT x = desc.Width / 20; x < desc.Width; x += desc.Width / 20 + 1)
            {
                const UINT32 pixel = static_cast<const UINT32*>(mapped.pData)[y * (mapped.RowPitch / 4) + x] & 0x00FFFFFF;
                ++counts[pixel];
                ++samples;
            }
        context->Unmap(chain.detectStaging.Get(), 0);
        UINT32 best = 0, second = 0;
        unsigned bestCount = 0, secondCount = 0;
        for (const auto& [pixel, count] : counts)
        {
            if (count > bestCount) { second = best; secondCount = bestCount; best = pixel; bestCount = count; }
            else if (count > secondCount) { second = pixel; secondCount = count; }
        }
        // A first color needs a clear plurality; replacing a known one needs a majority, so a dialog or a dark page
        // passing by doesn't switch the key.
        if (bestCount * (chain.haveBackground ? 2 : 4) < samples) return;
        const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
        auto toRgb = [bgra](UINT32 pixel, float* rgb) {
            const float c0 = (pixel & 0xFF) / 255.f, c1 = ((pixel >> 8) & 0xFF) / 255.f, c2 = ((pixel >> 16) & 0xFF) / 255.f;
            rgb[0] = bgra ? c2 : c0;
            rgb[1] = c1;
            rgb[2] = bgra ? c0 : c2;
        };
        float found[3];
        toRgb(best, found);
        // A second flat color covering a tenth of the window is keyed too (where it surrounds the pixel).
        const bool hadSecond = chain.haveBackground2;
        // Only a color of the same family: a dark app's black sidebar, never its white text or a light panel (keying
        // those washes the window out).
        float candidate[3];
        toRgb(second, candidate);
        auto light = [](const float* rgb) { return std::max({ rgb[0], rgb[1], rgb[2] }) > 0.5f; };
        chain.haveBackground2 = secondCount * 10 >= samples && light(candidate) == light(found);
        if (chain.haveBackground2)
        {
            float previous[3] = { chain.background2[0], chain.background2[1], chain.background2[2] };
            for (int i = 0; i < 3; ++i) chain.background2[i] = candidate[i];
            if (!hadSecond || previous[0] != chain.background2[0] || previous[1] != chain.background2[1] || previous[2] != chain.background2[2])
                Log(L"dxgi: second background #%02X%02X%02X (%u%% of samples)", int(candidate[0] * 255 + .5f), int(candidate[1] * 255 + .5f),
                    int(candidate[2] * 255 + .5f), secondCount * 100 / samples);
        }
        const bool changed = !chain.haveBackground || found[0] != chain.background[0] || found[1] != chain.background[1] || found[2] != chain.background[2];
        for (int i = 0; i < 3; ++i) chain.background[i] = found[i];
        if (changed)
            Log(L"dxgi: background #%02X%02X%02X (%u%% of samples)", int(chain.background[0] * 255 + .5f), int(chain.background[1] * 255 + .5f),
                int(chain.background[2] * 255 + .5f), bestCount * 100 / samples);
        chain.haveBackground = true;
    }

    void Process(IDXGISwapChain* swapChain, UINT flags)
    {
        if (flags & DXGI_PRESENT_TEST) return;
        AcquireSRWLockExclusive(&g_lock);
        auto& chain = g_chains[swapChain];
        if (chain.unsupported) { ReleaseSRWLockExclusive(&g_lock); return; }
        if (const auto window = g_chainWindows.find(swapChain); window != g_chainWindows.end() && IsFullscreen(window->second))
        {
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11Texture2D> frame;
        DXGI_SWAP_CHAIN_DESC desc{};
        swapChain->GetDesc(&desc);
        if (!chain.logged)
        {
            chain.logged = true;
            ComPtr<IDXGISwapChain1> chain1;
            DXGI_SWAP_CHAIN_DESC1 desc1{};
            HWND hwnd = nullptr;
            if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&chain1)))) { chain1->GetDesc1(&desc1); chain1->GetHwnd(&hwnd); }
            Log(L"dxgi: swap chain %p %ux%u format %d effect %d alpha mode %d hwnd %p", swapChain, desc.BufferDesc.Width, desc.BufferDesc.Height,
                desc.BufferDesc.Format, desc.SwapEffect, desc1.AlphaMode, hwnd);
        }
        if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))) || !Is8Bit(desc.BufferDesc.Format) || desc.SampleDesc.Count != 1
            || FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&frame))))
        {
            Log(L"dxgi: swap chain %p unsupported (not D3D11, not 8-bit UNORM, or multisampled); left alone", swapChain);
            chain.unsupported = true;
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
        auto& resources = ResourcesFor(device.Get());
        ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        ComPtr<ID3D11DeviceContext1> context1;
        if (!resources.ok || FAILED(context.As(&context1))) { ReleaseSRWLockExclusive(&g_lock); return; }

        D3D11_TEXTURE2D_DESC frameDesc{};
        frame->GetDesc(&frameDesc);
        if (!chain.copy || chain.width != frameDesc.Width || chain.height != frameDesc.Height || chain.format != frameDesc.Format)
        {
            D3D11_TEXTURE2D_DESC copyDesc = frameDesc;
            copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            copyDesc.MiscFlags = 0;
            copyDesc.Usage = D3D11_USAGE_DEFAULT;
            copyDesc.CPUAccessFlags = 0;
            chain.copy.Reset();
            chain.copyView.Reset();
            if (FAILED(device->CreateTexture2D(&copyDesc, nullptr, &chain.copy)) || FAILED(device->CreateShaderResourceView(chain.copy.Get(), nullptr, &chain.copyView)))
            {
                chain.unsupported = true;
                ReleaseSRWLockExclusive(&g_lock);
                return;
            }
            chain.width = frameDesc.Width;
            chain.height = frameDesc.Height;
            chain.format = frameDesc.Format;
        }
        DetectBackground(device.Get(), context.Get(), frame.Get(), chain, DetectNow(chain.frames++));
        if (!chain.haveBackground) { ReleaseSRWLockExclusive(&g_lock); return; }

        ComPtr<ID3D11RenderTargetView> target;
        if (FAILED(device->CreateRenderTargetView(frame.Get(), nullptr, &target))) { chain.unsupported = true; ReleaseSRWLockExclusive(&g_lock); return; }
        context->CopyResource(chain.copy.Get(), frame.Get());
        Params params{ { chain.background[0], chain.background[1], chain.background[2] }, BackgroundFloor };
        if (chain.haveBackground2)
        {
            for (int i = 0; i < 3; ++i) params.background2[i] = chain.background2[i];
            params.background2[3] = 1;
        }
        ReadKeepAreas(params);
        TrimKeepAreas(device.Get(), context.Get(), frame.Get(), chain, params);

        ComPtr<ID3DDeviceContextState> previous;
        context1->SwapDeviceContextState(resources.state.Get(), &previous);
        context->UpdateSubresource(resources.params.Get(), 0, nullptr, &params, 0, 0);
        const D3D11_VIEWPORT viewport{ 0, 0, float(frameDesc.Width), float(frameDesc.Height), 0, 1 };
        context->RSSetViewports(1, &viewport);
        context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
        context->OMSetBlendState(resources.noBlend.Get(), nullptr, 0xFFFFFFFF);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(resources.vs.Get(), nullptr, 0);
        context->PSSetShader(resources.ps.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, chain.copyView.GetAddressOf());
        context->PSSetConstantBuffers(0, 1, resources.params.GetAddressOf());
        context->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        context->PSSetShaderResources(0, 1, &none);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context1->SwapDeviceContextState(previous.Get(), nullptr);
        ReleaseSRWLockExclusive(&g_lock);
    }

    HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* swapChain, UINT sync, UINT flags)
    {
        Process(swapChain, flags);
        return Real_Present(swapChain, sync, flags);
    }

    HRESULT STDMETHODCALLTYPE Hook_Present1(IDXGISwapChain1* swapChain, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters)
    {
        Process(swapChain, flags);
        return Real_Present1(swapChain, sync, flags, parameters);
    }

    std::map<HWND, WNDPROC> g_originalProcs;

    // Qt (and others) reset the frame margins themselves, on activation and resize; the window's own white surface then
    // shows wherever the frame is see-through. For kept windows, every such call extends the frame over the whole window.
    using ExtendFrameFn = HRESULT(WINAPI*)(HWND, const MARGINS*);
    ExtendFrameFn Real_DwmExtendFrameIntoClientArea = DwmExtendFrameIntoClientArea;

    HRESULT WINAPI Hook_DwmExtendFrameIntoClientArea(HWND window, const MARGINS* margins)
    {
        AcquireSRWLockShared(&g_windowLock);
        const bool kept = g_originalProcs.count(window) != 0;
        ReleaseSRWLockShared(&g_windowLock);
        if (!kept) return Real_DwmExtendFrameIntoClientArea(window, margins);
        static const MARGINS whole{ -1, -1, -1, -1 };
        return Real_DwmExtendFrameIntoClientArea(window, &whole);
    }

    LRESULT CALLBACK KeepFrameActive(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        AcquireSRWLockShared(&g_windowLock);
        const auto it = g_originalProcs.find(window);
        const WNDPROC original = it == g_originalProcs.end() ? DefWindowProcW : it->second;
        ReleaseSRWLockShared(&g_windowLock);
        if (message == WM_NCACTIVATE) return CallWindowProcW(original, window, message, TRUE, lParam);
        // Under the composition layer the window keeps its ordinary GDI surface. The app never paints it (it draws
        // with DXGI), so when the window grows Windows fills the new part white, which shows through every see-through
        // pixel as a white box. Black there is fully transparent in the extended frame.
        if (message == WM_ERASEBKGND)
        {
            RECT client{};
            GetClientRect(window, &client);
            FillRect(reinterpret_cast<HDC>(wParam), &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            return 1;
        }
        if (message == WM_SIZE)
        {
            const LRESULT result = CallWindowProcW(original, window, message, wParam, lParam);
            RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ERASENOW);
            return result;
        }
        if (message == WM_NCDESTROY)
        {
            SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
            AcquireSRWLockExclusive(&g_windowLock);
            g_originalProcs.erase(window);
            ReleaseSRWLockExclusive(&g_windowLock);
        }
        return CallWindowProcW(original, window, message, wParam, lParam);
    }

    // Called with g_windowLock held exclusively. Window procedures can be swapped from any thread of the process, which
    // matters because apps often create their swap chain off the window's thread.
    void KeepActiveLocked(HWND window)
    {
        if (g_originalProcs.count(window)) return;
        HMODULE self = nullptr; // a window procedure must never outlive the DLL
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&KeepFrameActive), &self);
        g_originalProcs[window] = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(KeepFrameActive)));
        PostMessageW(window, WM_NCACTIVATE, TRUE, 0);
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE);
        // The glass itself: live Acrylic behind the whole window. Set once from inside the app, so nothing outside
        // has to keep re-applying it on focus changes.
        const int acrylic = 3; // DWMSBT_TRANSIENTWINDOW
        const MARGINS whole{ -1, -1, -1, -1 };
        // Dark: Acrylic follows the window's dark-mode flag, and a light backdrop washes out an app's light text.
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
        const HRESULT backdrop = DwmSetWindowAttribute(window, DWMWA_SYSTEMBACKDROP_TYPE, &acrylic, sizeof acrylic);
        const HRESULT extend = Real_DwmExtendFrameIntoClientArea(window, &whole);
        Log(L"dxgi: window %p kept drawn as active; acrylic %08X, whole-window frame %08X", window, backdrop, extend);
    }

    bool ComposableFormat(DXGI_FORMAT format)
    {
        return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R16G16B16A16_FLOAT
            || format == DXGI_FORMAT_R10G10B10A2_UNORM;
    }

    bool ShowOnWindow(HWND window, IDXGISwapChain1* chain)
    {
        WindowVisual tree;
        if (FAILED(DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&tree.device))) || FAILED(tree.device->CreateTargetForHwnd(window, TRUE, &tree.target))
            || FAILED(tree.device->CreateVisual(&tree.visual)) || FAILED(tree.visual->SetContent(chain)) || FAILED(tree.target->SetRoot(tree.visual.Get()))
            || FAILED(tree.device->Commit()))
            return false;
        g_visuals.push_back(std::move(tree));
        return true;
    }

    // Makes the composition twin of a window swap chain; false leaves the app's own request untouched.
    bool CreateComposed(IDXGIFactory2* factory, IUnknown* device, HWND window, DXGI_SWAP_CHAIN_DESC1 desc, IDXGISwapChain1** chain)
    {
        if (!window || desc.SampleDesc.Count != 1 || !ComposableFormat(desc.Format)) return false;
        if (desc.Width == 0 || desc.Height == 0)
        {
            RECT client{};
            GetClientRect(window, &client);
            desc.Width = desc.Width ? desc.Width : static_cast<UINT>(std::max<LONG>(client.right, 8));
            desc.Height = desc.Height ? desc.Height : static_cast<UINT>(std::max<LONG>(client.bottom, 8));
        }
        if (desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD && desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.BufferCount = std::max<UINT>(desc.BufferCount, 2);
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        desc.Flags &= ~static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
        ComPtr<IDXGISwapChain1> composed;
        const HRESULT created = Real_CreateSwapChainForComposition(factory, device, &desc, nullptr, &composed);
        if (FAILED(created) || !ShowOnWindow(window, composed.Get()))
        {
            Log(L"dxgi: composition swap chain for %p failed (%08X); using the app's own", window, created);
            return false;
        }
        Log(L"dxgi: window %p now shows a composition swap chain %p (%ux%u format %d)", window, composed.Get(), desc.Width, desc.Height, desc.Format);
        AcquireSRWLockExclusive(&g_windowLock);
        KeepActiveLocked(GetAncestor(window, GA_ROOT));
        ReleaseSRWLockExclusive(&g_windowLock);
        AcquireSRWLockExclusive(&g_lock);
        g_chainWindows[composed.Get()] = window;
        ReleaseSRWLockExclusive(&g_lock);
        *chain = composed.Detach();
        return true;
    }

    HRESULT STDMETHODCALLTYPE Hook_CreateSwapChainForHwnd(IDXGIFactory2* factory, IUnknown* device, HWND window, const DXGI_SWAP_CHAIN_DESC1* desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* output, IDXGISwapChain1** chain)
    {
        if (desc && chain && (!fullscreen || fullscreen->Windowed) && CreateComposed(factory, device, window, *desc, chain)) return S_OK;
        return Real_CreateSwapChainForHwnd(factory, device, window, desc, fullscreen, output, chain);
    }

    // Chromium's GPU process already draws through DirectComposition, but asks for its swap chains with alpha ignored.
    HRESULT STDMETHODCALLTYPE Hook_CreateSwapChainForComposition(IDXGIFactory2* factory, IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc,
        IDXGIOutput* output, IDXGISwapChain1** chain)
    {
        if (desc && (desc->AlphaMode == DXGI_ALPHA_MODE_IGNORE || desc->AlphaMode == DXGI_ALPHA_MODE_UNSPECIFIED) && ComposableFormat(desc->Format))
        {
            DXGI_SWAP_CHAIN_DESC1 premultiplied = *desc;
            premultiplied.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
            const HRESULT result = Real_CreateSwapChainForComposition(factory, device, &premultiplied, output, chain);
            Log(L"dxgi: composition swap chain %ux%u format %d made premultiplied (%08X)", desc->Width, desc->Height, desc->Format, result);
            if (SUCCEEDED(result)) return result;
        }
        return Real_CreateSwapChainForComposition(factory, device, desc, output, chain);
    }

    // Chromium draws in a separate GPU process that the main process starts. That child gets the hook at birth: it is
    // created suspended, its import table gains this DLL (Detours), and only then does it run. Renderers are sandboxed
    // and draw nothing to the screen, so only the GPU process is touched.
    using CreateProcessWFn = BOOL(WINAPI*)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR,
        LPSTARTUPINFOW, LPPROCESS_INFORMATION);
    using CreateProcessAsUserWFn = BOOL(WINAPI*)(HANDLE, LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID,
        LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
    CreateProcessWFn Real_CreateProcessW = CreateProcessW;
    CreateProcessAsUserWFn Real_CreateProcessAsUserW = CreateProcessAsUserW;

    bool IsGpuProcess(LPCWSTR commandLine)
    {
        return commandLine && wcsstr(commandLine, L"--type=gpu-process");
    }

    void InjectChild(const PROCESS_INFORMATION* process, bool resume)
    {
        char dll[MAX_PATH]{};
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&InjectChild), &self);
        GetModuleFileNameA(self, dll, MAX_PATH);
        LPCSTR dlls[] = { dll };
        const BOOL ok = DetourUpdateProcessWithDll(process->hProcess, dlls, 1);
        Log(L"dxgi: GPU process %lu %s", process->dwProcessId, ok ? L"gets the hook" : L"could NOT be hooked");
        if (resume) ResumeThread(process->hThread);
    }

    BOOL WINAPI Hook_CreateProcessW(LPCWSTR application, LPWSTR commandLine, LPSECURITY_ATTRIBUTES processAttributes,
        LPSECURITY_ATTRIBUTES threadAttributes, BOOL inherit, DWORD flags, LPVOID environment, LPCWSTR directory, LPSTARTUPINFOW startup,
        LPPROCESS_INFORMATION process)
    {
        const bool gpu = IsGpuProcess(commandLine);
        const bool resume = gpu && !(flags & CREATE_SUSPENDED);
        const BOOL ok = Real_CreateProcessW(application, commandLine, processAttributes, threadAttributes, inherit,
            gpu ? flags | CREATE_SUSPENDED : flags, environment, directory, startup, process);
        if (ok && gpu) InjectChild(process, resume);
        return ok;
    }

    BOOL WINAPI Hook_CreateProcessAsUserW(HANDLE token, LPCWSTR application, LPWSTR commandLine, LPSECURITY_ATTRIBUTES processAttributes,
        LPSECURITY_ATTRIBUTES threadAttributes, BOOL inherit, DWORD flags, LPVOID environment, LPCWSTR directory, LPSTARTUPINFOW startup,
        LPPROCESS_INFORMATION process)
    {
        const bool gpu = IsGpuProcess(commandLine);
        const bool resume = gpu && !(flags & CREATE_SUSPENDED);
        const BOOL ok = Real_CreateProcessAsUserW(token, application, commandLine, processAttributes, threadAttributes, inherit,
            gpu ? flags | CREATE_SUSPENDED : flags, environment, directory, startup, process);
        if (ok && gpu) InjectChild(process, resume);
        return ok;
    }

    HRESULT STDMETHODCALLTYPE Hook_CreateSwapChain(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** chain)
    {
        ComPtr<IDXGIFactory2> factory2;
        if (desc && chain && desc->Windowed && SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory2))))
        {
            const DXGI_SWAP_CHAIN_DESC1 desc1{ desc->BufferDesc.Width, desc->BufferDesc.Height, desc->BufferDesc.Format, FALSE, desc->SampleDesc,
                desc->BufferUsage, desc->BufferCount, DXGI_SCALING_STRETCH, desc->SwapEffect, DXGI_ALPHA_MODE_PREMULTIPLIED, desc->Flags };
            IDXGISwapChain1* composed = nullptr;
            if (CreateComposed(factory2.Get(), device, desc->OutputWindow, desc1, &composed)) { *chain = composed; return S_OK; }
        }
        return Real_CreateSwapChain(factory, device, desc, chain);
    }

    // Present/Present1 live in dxgi.dll and are shared by every swap chain, so one throwaway chain gives their addresses.
    bool FindPresent()
    {
        ComPtr<ID3D11Device> device;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr)))
            return false;
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;
        DXGI_SWAP_CHAIN_DESC1 desc{ 16, 16, DXGI_FORMAT_B8G8R8A8_UNORM, FALSE, { 1, 0 }, DXGI_USAGE_RENDER_TARGET_OUTPUT, 2, DXGI_SCALING_STRETCH,
            DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, DXGI_ALPHA_MODE_PREMULTIPLIED, 0 };
        ComPtr<IDXGISwapChain1> chain;
        if (FAILED(factory->CreateSwapChainForComposition(device.Get(), &desc, nullptr, &chain))) return false;
        void** table = *reinterpret_cast<void***>(chain.Get());
        Real_Present = reinterpret_cast<PresentFn>(table[8]);
        Real_Present1 = reinterpret_cast<Present1Fn>(table[22]);
        void** factoryTable = *reinterpret_cast<void***>(factory.Get());
        Real_CreateSwapChain = reinterpret_cast<CreateLegacyFn>(factoryTable[10]);
        Real_CreateSwapChainForHwnd = reinterpret_cast<CreateForHwndFn>(factoryTable[15]);
        Real_CreateSwapChainForComposition = reinterpret_cast<CreateForCompositionFn>(factoryTable[24]);
        return true;
    }

    bool InList(const wchar_t* file, const wchar_t* exe)
    {
        HANDLE handle = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return false;
        char buffer[4096]{};
        DWORD read = 0;
        ReadFile(handle, buffer, sizeof buffer - 1, &read, nullptr);
        CloseHandle(handle);
        wchar_t text[4096]{};
        MultiByteToWideChar(CP_UTF8, 0, buffer, -1, text, _countof(text));
        wchar_t* context = nullptr;
        for (wchar_t* line = wcstok_s(text, L"\r\n", &context); line; line = wcstok_s(nullptr, L"\r\n", &context))
            if (_wcsicmp(line, exe) == 0) return true;
        return false;
    }
}

bool DxgiGlassRequested()
{
    wchar_t list[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", list, MAX_PATH)) return false;
    wcscat_s(list, L"\\GlassForge\\dxgi.txt");
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    return InList(list, name ? name + 1 : path);
}

void InstallDxgiGlass()
{
    AcquireSRWLockExclusive(&g_lock);
    if (!g_installed && FindPresent())
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(reinterpret_cast<PVOID*>(&Real_Present), reinterpret_cast<PVOID>(Hook_Present));
        DetourAttach(reinterpret_cast<PVOID*>(&Real_Present1), reinterpret_cast<PVOID>(Hook_Present1));
        DetourAttach(reinterpret_cast<PVOID*>(&Real_CreateSwapChain), reinterpret_cast<PVOID>(Hook_CreateSwapChain));
        DetourAttach(reinterpret_cast<PVOID*>(&Real_CreateSwapChainForHwnd), reinterpret_cast<PVOID>(Hook_CreateSwapChainForHwnd));
        DetourAttach(reinterpret_cast<PVOID*>(&Real_CreateSwapChainForComposition), reinterpret_cast<PVOID>(Hook_CreateSwapChainForComposition));
        DetourAttach(reinterpret_cast<PVOID*>(&Real_CreateProcessW), reinterpret_cast<PVOID>(Hook_CreateProcessW));
        DetourAttach(reinterpret_cast<PVOID*>(&Real_CreateProcessAsUserW), reinterpret_cast<PVOID>(Hook_CreateProcessAsUserW));
        DetourAttach(reinterpret_cast<PVOID*>(&Real_DwmExtendFrameIntoClientArea), reinterpret_cast<PVOID>(Hook_DwmExtendFrameIntoClientArea));
        const LONG result = DetourTransactionCommit();
        g_installed = result == NO_ERROR;
        Log(L"dxgi: present and swap chain detours %s (%ld)", g_installed ? L"installed" : L"FAILED", result);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void RemoveDxgiGlass()
{
    AcquireSRWLockExclusive(&g_lock);
    if (g_installed)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(reinterpret_cast<PVOID*>(&Real_Present), reinterpret_cast<PVOID>(Hook_Present));
        DetourDetach(reinterpret_cast<PVOID*>(&Real_Present1), reinterpret_cast<PVOID>(Hook_Present1));
        DetourDetach(reinterpret_cast<PVOID*>(&Real_CreateSwapChain), reinterpret_cast<PVOID>(Hook_CreateSwapChain));
        DetourDetach(reinterpret_cast<PVOID*>(&Real_CreateSwapChainForHwnd), reinterpret_cast<PVOID>(Hook_CreateSwapChainForHwnd));
        DetourDetach(reinterpret_cast<PVOID*>(&Real_CreateSwapChainForComposition), reinterpret_cast<PVOID>(Hook_CreateSwapChainForComposition));
        DetourDetach(reinterpret_cast<PVOID*>(&Real_CreateProcessW), reinterpret_cast<PVOID>(Hook_CreateProcessW));
        DetourDetach(reinterpret_cast<PVOID*>(&Real_CreateProcessAsUserW), reinterpret_cast<PVOID>(Hook_CreateProcessAsUserW));
        DetourDetach(reinterpret_cast<PVOID*>(&Real_DwmExtendFrameIntoClientArea), reinterpret_cast<PVOID>(Hook_DwmExtendFrameIntoClientArea));
        g_installed = DetourTransactionCommit() != NO_ERROR;
        g_chains.clear();
        g_devices.clear();
        if (g_keepView) UnmapViewOfFile(g_keepView);
        if (g_keepMapping) CloseHandle(g_keepMapping);
        g_keepView = nullptr;
        g_keepMapping = nullptr;
        Log(L"dxgi: present detours removed");
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// Started from DllMain when the hook is in the process from launch: the swap chain substitution only helps if it is in
// place before the app makes its first swap chain. DllMain holds the loader lock, so the setup runs on its own thread.
void StartDxgiGlassEarly()
{
    if (HANDLE thread = CreateThread(nullptr, 0, [](void*) -> DWORD { InstallDxgiGlass(); return 0; }, nullptr, 0, nullptr))
        CloseHandle(thread);
}

// rundll32 GlassForge.Hook.dll,LaunchWithGlass <command line>: starts an app with the hook loaded from its first
// instruction (Detours rewrites the new process's import table), for apps whose swap chain must be caught at creation.
extern "C" void CALLBACK LaunchWithGlassW(HWND, HINSTANCE, LPWSTR commandLine, int show)
{
    char dll[MAX_PATH]{};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&LaunchWithGlassW), &self);
    GetModuleFileNameA(self, dll, MAX_PATH);
    STARTUPINFOW startup{ sizeof startup };
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = static_cast<WORD>(show);
    PROCESS_INFORMATION process{};
    const BOOL ok = DetourCreateProcessWithDllExW(nullptr, commandLine, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process, dll, nullptr);
    Log(L"launch with glass: %s -> %s (pid %lu)", commandLine, ok ? L"started" : L"FAILED", process.dwProcessId);
    if (ok) { CloseHandle(process.hThread); CloseHandle(process.hProcess); }
}
