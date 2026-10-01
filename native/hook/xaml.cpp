// Explorer's WinUI backgrounds require visual-tree properties, not GDI hooks.
// Uses the Windows SDK diagnostics interfaces without a WinUI package dependency.
#include <windows.h>
#include <ocidl.h>
#include <xamlom.h>
#include <wrl.h>
#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include "xaml.h"
#include "log.h"

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::Make;

namespace {
// GlassForge-specific COM class; no registration or system-wide settings.
constexpr CLSID TapClass = {0x3546f878,0x8a67,0x4301,{0xb9,0x21,0x21,0x51,0x31,0x8c,0x45,0x29}};
std::atomic<bool> active{false};
std::atomic<bool> connecting{false};
HMODULE ownModule = nullptr;
// Every Explorer window has its own UI thread; only elements on hooked threads are styled.
SRWLOCK threadLock = SRWLOCK_INIT;
std::map<DWORD, XamlStyle> targetThreads;
class Tap;
SRWLOCK tapLock = SRWLOCK_INIT;
std::vector<ComPtr<Tap>> taps;

bool IsTargetThread(DWORD thread, XamlStyle* style = nullptr) {
    AcquireSRWLockShared(&threadLock);
    const auto entry = targetThreads.find(thread);
    const bool found = entry != targetThreads.end();
    if (found && style) *style = entry->second;
    ReleaseSRWLockShared(&threadLock);
    return found;
}

class Tap final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IObjectWithSite, IVisualTreeServiceCallback> {
    ComPtr<IUnknown> site;
    ComPtr<IVisualTreeService2> service;
    ComPtr<IXamlDiagnostics> diagnostics;
    struct Node { InstanceHandle parent; std::wstring type, name; DWORD thread; };
    // Callbacks arrive on each element's own thread, and SetProperty can call straight back in.
    std::recursive_mutex lock;
    std::map<InstanceHandle, Node> nodes;
    struct Saved { InstanceHandle element; unsigned index; InstanceHandle original; bool local; ComPtr<IInspectable> keep; DWORD thread; };
    std::vector<Saved> saved;
    // XAML objects belong to the thread that created them, and every Explorer window has its own XAML thread:
    // a brush made on one window's thread can't be set on another's (SetProperty fails with 80070490).
    std::map<DWORD, InstanceHandle> transparentByThread;
    unsigned logged = 0;

    bool CommandContainer(InstanceHandle element, const Node& node) {
        if (node.type == L"FileExplorerExtensions.GalleryViewControl" || node.name == L"GalleryRootGrid") return true;
        // Only Gallery's host chain, never the thumbnail/item templates.
        if (node.type == L"Microsoft.UI.Xaml.Controls.Grid" || node.type == L"Microsoft.UI.Xaml.Controls.Border" ||
            node.type == L"Microsoft.UI.Xaml.Internal.RootScrollViewer" || node.type == L"Microsoft.UI.Xaml.Controls.ScrollViewer") {
            for (const auto& [handle, item] : nodes) {
                if (item.type != L"FileExplorerExtensions.GalleryViewControl") continue;
                InstanceHandle parent = item.parent;
                for (int depth=0; parent && depth<8; ++depth) {
                    if (parent == element) return true;
                    auto ancestor = nodes.find(parent);
                    if (ancestor == nodes.end()) break;
                    parent = ancestor->second.parent;
                }
                auto immediate = nodes.find(node.parent);
                if (node.parent == handle || (immediate != nodes.end() && immediate->second.parent == handle)) return true;
            }
        }
        // Home, the details pane and the address/navigation bar each paint one solid root grid.
        if (node.type == L"Microsoft.UI.Xaml.Controls.Grid" && (node.name == L"HomeViewRootGrid" ||
            node.name == L"DetailsViewControlRootGrid" || node.name == L"NavigationBarControlGrid")) return true;
        if (node.type == L"FileExplorerExtensions.CommandBarControl") return true;
        if (node.name == L"CommandBarControlRootGrid") return true;
        if (node.type == L"Microsoft.UI.Xaml.Controls.CommandBar" &&
            (node.name == L"FileExplorerCommandBar" || node.name == L"FileExplorerSecondaryCommandBar")) return true;
        if (node.type == L"Microsoft.UI.Xaml.Controls.Grid" && (node.name == L"LayoutRoot" || node.name == L"ContentRoot")) {
            auto parent = nodes.find(node.parent);
            if (parent != nodes.end() && parent->second.type == L"Microsoft.UI.Xaml.Controls.CommandBar") return true;
            if (parent != nodes.end() && parent->second.name == L"LayoutRoot") {
                auto grand = nodes.find(parent->second.parent);
                return grand != nodes.end() && grand->second.type == L"Microsoft.UI.Xaml.Controls.CommandBar";
            }
        }
        return false;
    }

    void Style(InstanceHandle element, const Node& node) {
        if (!CommandContainer(element, node)) return;
        InstanceHandle& transparent = transparentByThread[node.thread];
        for (const auto& record : saved) {
            if (record.element != element) continue;
            InstanceHandle current = 0;
            if (SUCCEEDED(service->GetProperty(element, record.index, &current)) && current != transparent)
                service->SetProperty(element, transparent, record.index);
            return;
        }
        unsigned index = 0;
        bool found = false;
        std::wstring brushType;
        Saved record{element, 0, 0, false, {}, node.thread};
        unsigned sourceCount = 0, propertyCount = 0;
        PropertyChainSource* sources = nullptr;
        PropertyChainValue* properties = nullptr;
        if (SUCCEEDED(service->GetPropertyValuesChain(element, &sourceCount, &sources, &propertyCount, &properties))) {
            for (unsigned i=0; i<propertyCount; ++i) {
                const auto& property = properties[i];
                if (property.PropertyName && wcscmp(property.PropertyName, L"Background") == 0) {
                    index = property.Index;
                    found = true;
                    if (property.Type) brushType = property.Type;
                }
                if (property.PropertyName && wcscmp(property.PropertyName, L"Background") == 0 && property.PropertyChainIndex < sourceCount &&
                    sources[property.PropertyChainIndex].Source == BaseValueSourceLocal) record.local = true;
                SysFreeString(property.Type); SysFreeString(property.DeclaringType); SysFreeString(property.ValueType);
                SysFreeString(property.ItemType); SysFreeString(property.Value); SysFreeString(property.PropertyName);
            }
            for (unsigned i=0; i<sourceCount; ++i) {
                SysFreeString(sources[i].TargetType); SysFreeString(sources[i].Name);
                SysFreeString(sources[i].SrcInfo.FileName); SysFreeString(sources[i].SrcInfo.Hash);
            }
            CoTaskMemFree(properties); CoTaskMemFree(sources);
        }
        if (!found) { Log(L"XAML no Background on %s#%s", node.type.c_str(), node.name.c_str()); return; }
        record.index = index;
        const HRESULT originalResult = service->GetProperty(element, index, &record.original);
        if (FAILED(originalResult) && record.local) { Log(L"XAML cannot save local brush %08X", originalResult); return; }
        if (record.original) diagnostics->GetIInspectableFromHandle(record.original, &record.keep);
        HRESULT result;
        if (!transparent) {
            BSTR type = SysAllocString(brushType.c_str());
            BSTR text = SysAllocString(L"Transparent");
            result = service->CreateInstance(type, text, &transparent);
            SysFreeString(type); SysFreeString(text);
            Log(L"XAML transparent brush thread=%lu type=%s %08X handle=%llu", node.thread, brushType.c_str(), result, transparent);
            if (FAILED(result)) return;
        }
        result = service->SetProperty(element, transparent, index);
        Log(L"XAML background %s#%s %08X", node.type.c_str(), node.name.c_str(), result);
        if (SUCCEEDED(result)) saved.push_back(std::move(record));
    }
    // Survey aid: a brush's Color and Opacity, read from its own property chain.
    std::wstring BrushColor(InstanceHandle brush) {
        std::wstring text;
        unsigned sourceCount = 0, propertyCount = 0;
        PropertyChainSource* sources = nullptr;
        PropertyChainValue* properties = nullptr;
        if (!brush || FAILED(service->GetPropertyValuesChain(brush, &sourceCount, &sources, &propertyCount, &properties))) return L"?";
        for (unsigned i=0; i<propertyCount; ++i) {
            const auto& property = properties[i];
            if (property.PropertyName && property.Value && (wcscmp(property.PropertyName, L"Color") == 0 ||
                wcscmp(property.PropertyName, L"Opacity") == 0 || wcscmp(property.PropertyName, L"TintColor") == 0))
                text += std::wstring(property.PropertyName) + L":" + property.Value + L" ";
            SysFreeString(property.Type); SysFreeString(property.DeclaringType); SysFreeString(property.ValueType);
            SysFreeString(property.ItemType); SysFreeString(property.Value); SysFreeString(property.PropertyName);
        }
        for (unsigned i=0; i<sourceCount; ++i) {
            SysFreeString(sources[i].TargetType); SysFreeString(sources[i].Name);
            SysFreeString(sources[i].SrcInfo.FileName); SysFreeString(sources[i].SrcInfo.Hash);
        }
        CoTaskMemFree(properties); CoTaskMemFree(sources);
        return text;
    }

    // Survey aid: every element whose Background (or Fill) is set to something, with its value type and source.
    void LogBackground(InstanceHandle element, const Node& node) {
        unsigned sourceCount = 0, propertyCount = 0;
        PropertyChainSource* sources = nullptr;
        PropertyChainValue* properties = nullptr;
        if (FAILED(service->GetPropertyValuesChain(element, &sourceCount, &sources, &propertyCount, &properties))) return;
        for (unsigned i=0; i<propertyCount; ++i) {
            const auto& property = properties[i];
            const bool brush = property.PropertyName && (wcscmp(property.PropertyName, L"Background") == 0 || wcscmp(property.PropertyName, L"Fill") == 0);
            if (brush && property.Value && wcscmp(property.Value, L"0") != 0 && property.PropertyChainIndex < sourceCount)
                Log(L"XAML brush %s#%s %s=%s (%s source %d)", node.type.c_str(), node.name.c_str(), property.PropertyName,
                    BrushColor(_wcstoui64(property.Value, nullptr, 10)).c_str(), property.ValueType ? property.ValueType : L"?",
                    static_cast<int>(sources[property.PropertyChainIndex].Source));
            SysFreeString(property.Type); SysFreeString(property.DeclaringType); SysFreeString(property.ValueType);
            SysFreeString(property.ItemType); SysFreeString(property.Value); SysFreeString(property.PropertyName);
        }
        for (unsigned i=0; i<sourceCount; ++i) {
            SysFreeString(sources[i].TargetType); SysFreeString(sources[i].Name);
            SysFreeString(sources[i].SrcInfo.FileName); SysFreeString(sources[i].SrcInfo.Hash);
        }
        CoTaskMemFree(properties); CoTaskMemFree(sources);
    }
public:
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* newSite) override {
        site = newSite;
        if (!newSite) return S_OK;
        HRESULT result = newSite->QueryInterface(IID_PPV_ARGS(&service));
        newSite->QueryInterface(IID_PPV_ARGS(&diagnostics));
        Log(L"XAML SetSite thread %lu service %08X", GetCurrentThreadId(), result);
        if (FAILED(result)) return result;
        AcquireSRWLockExclusive(&tapLock);
        taps.emplace_back(this);
        ReleaseSRWLockExclusive(&tapLock);
        // Advise enumerates existing elements and must not block the UI thread.
        AddRef();
        const HANDLE worker = CreateThread(nullptr, 0, [](void* context)->DWORD {
            auto* tap = static_cast<Tap*>(context);
            const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const HRESULT advised = tap->service->AdviseVisualTreeChange(tap);
            Log(L"XAML advise %08X", advised);
            if (SUCCEEDED(initialized)) CoUninitialize();
            tap->Release();
            return 0;
        }, this, 0, nullptr);
        if (worker) CloseHandle(worker); else Release();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSite(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        return site ? site->QueryInterface(iid, result) : E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation relation, VisualElement element, VisualMutationType mutation) override {
        if (!active) return S_OK;
        std::lock_guard guard(lock);
        if (mutation == Remove) {
            nodes.erase(element.Handle);
            saved.erase(std::remove_if(saved.begin(), saved.end(), [&](const Saved& record){ return record.element == element.Handle; }), saved.end());
            return S_OK;
        }
        // Recorded even for threads not hooked yet, so a window hooked later still knows its existing elements.
        const DWORD thread = GetCurrentThreadId();
        Node node{relation.Parent, element.Type ? element.Type : L"", element.Name ? element.Name : L"", thread};
        nodes[element.Handle] = node;
        XamlStyle style;
        if (!IsTargetThread(thread, &style)) return S_OK;
        if (logged < 1400 && (style.logAll || node.type.find(L"FileExplorer") != std::wstring::npos ||
            node.type.find(L"Gallery") != std::wstring::npos)) {
            ++logged;
            Log(L"XAML node thread=%lu parent=%llu handle=%llu type=%s name=%s", thread,
                relation.Parent, element.Handle, node.type.c_str(), node.name.c_str());
        }
        if (diagnostics && style.app == XamlApp::Explorer) Style(element.Handle, node);
        if (diagnostics && style.logAll && style.app != XamlApp::Explorer) LogBackground(element.Handle, node);
        return S_OK;
    }
    // Only the calling thread's records: XAML properties must be set on the element's own thread.
    void Restore(DWORD thread) {
        std::lock_guard guard(lock);
        for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
            if (it->thread != thread) continue;
            const HRESULT result = it->local ? service->SetProperty(it->element, it->original, it->index) :
                service->ClearProperty(it->element, it->index);
            Log(L"XAML restore handle=%llu %08X", it->element, result);
        }
        saved.erase(std::remove_if(saved.begin(), saved.end(), [&](const Saved& record){ return record.thread == thread; }), saved.end());
        // Thread IDs are recycled; a new window on a reused ID needs a brush of its own.
        transparentByThread.erase(thread);
    }
    void Unadvise() { service->UnadviseVisualTreeChange(this); }
    void Refresh(DWORD thread) {
        if (!active) return;
        std::lock_guard guard(lock);
        for (const auto& [handle, node] : nodes) if (node.thread == thread) Style(handle, node);
    }
};

class Factory final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory> {
public:
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto tap = Make<Tap>();
        return tap ? tap->QueryInterface(iid, result) : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
};

DWORD WINAPI Connect(void*) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(ownModule, path, _countof(path));
    // WinUI 3 apps (Explorer) expose diagnostics from the Windows App SDK framework; UWP apps (Settings) from system
    // XAML, under a different connection name.
    HMODULE runtime = GetModuleHandleW(L"Microsoft.Internal.FrameworkUdk.dll");
    const wchar_t* prefix = L"WinUIVisualDiagConnection";
    if (!runtime) {
        runtime = GetModuleHandleW(L"Windows.UI.Xaml.dll");
        prefix = L"VisualDiagConnection";
    }
    const auto initialize = runtime ? reinterpret_cast<decltype(&InitializeXamlDiagnosticsEx)>(
        GetProcAddress(runtime, "InitializeXamlDiagnosticsEx")) : nullptr;
    HRESULT result = HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    if (initialize) {
        for (int index = 1; index <= 64 && active; ++index) {
            wchar_t connection[80];
            swprintf_s(connection, L"%s%d", prefix, index);
            result = initialize(connection, GetCurrentProcessId(), L"", path, TapClass, nullptr);
            if (result != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) break;
        }
    }
    Log(L"XAML diagnostics connection %08X", result);
    if (FAILED(result)) connecting = false;
    if (SUCCEEDED(initialized)) CoUninitialize();
    return 0;
}
}

void AddXamlThread(HMODULE module, DWORD thread, const XamlStyle& style) {
    ownModule = module;
    AcquireSRWLockExclusive(&threadLock);
    targetThreads[thread] = style;
    ReleaseSRWLockExclusive(&threadLock);
    active = true;
    // One diagnostics connection serves every thread in the process. It retains the TAP DLL; keep that lifetime
    // separate from the thread hook so a late callback never targets unloaded code.
    if (connecting.exchange(true)) { RefreshXamlStyling(); return; }
    const HANDLE worker = CreateThread(nullptr, 0, Connect, nullptr, 0, nullptr);
    if (worker) CloseHandle(worker); else connecting = false;
}

void RemoveXamlThread() {
    const DWORD thread = GetCurrentThreadId();
    AcquireSRWLockExclusive(&threadLock);
    targetThreads.erase(thread);
    ReleaseSRWLockExclusive(&threadLock);
    std::vector<ComPtr<Tap>> current;
    AcquireSRWLockShared(&tapLock);
    current = taps;
    ReleaseSRWLockShared(&tapLock);
    for (auto& tap : current) tap->Restore(thread);
}

void StopXamlStyling() {
    active = false;
    std::vector<ComPtr<Tap>> previous;
    AcquireSRWLockExclusive(&tapLock);
    previous.swap(taps);
    ReleaseSRWLockExclusive(&tapLock);
    connecting = false;
    if (previous.empty()) return;
    auto* pending = new std::vector<ComPtr<Tap>>(std::move(previous));
    const HANDLE worker = CreateThread(nullptr, 0, [](void* context)->DWORD {
        auto* list = static_cast<std::vector<ComPtr<Tap>>*>(context);
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (auto& tap : *list) tap->Unadvise();
        delete list;
        if (SUCCEEDED(initialized)) CoUninitialize();
        return 0;
    }, pending, 0, nullptr);
    if (worker) CloseHandle(worker); else delete pending;
}

void RefreshXamlStyling() {
    const DWORD thread = GetCurrentThreadId();
    if (!IsTargetThread(thread)) return;
    std::vector<ComPtr<Tap>> current;
    AcquireSRWLockShared(&tapLock);
    current = taps;
    ReleaseSRWLockShared(&tapLock);
    for (auto& tap : current) tap->Refresh(thread);
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** value) {
    if (!value) return E_POINTER;
    *value = nullptr;
    if (clsid != TapClass) return CLASS_E_CLASSNOTAVAILABLE;
    auto factory = Make<Factory>();
    return factory ? factory->QueryInterface(iid, value) : E_OUTOFMEMORY;
}

STDAPI DllCanUnloadNow() { return S_FALSE; }
