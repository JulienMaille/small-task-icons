// tray_resizer_dll.cpp — Standalone Taskbar Tray Icon Resizer
//
// Extracted from the Windhawk "Taskbar tray icon spacing and grid" mod by m417z.
// This is the worker DLL injected into explorer.exe.
//
// HOW TO BUILD (x64 Native Tools Command Prompt for VS 2022+):
//   cl /EHsc /MD /LD /std:c++20 /Zc:preprocessor tray_resizer_dll.cpp ^
//       /Fe:tray_resizer_dll.dll ^
//       /I"%WindowsSdkDir%Include\%WindowsSDKVersion%\cppwinrt" ^
//       /link windowsapp.lib ole32.lib oleaut32.lib runtimeobject.lib

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <psapi.h>
#include <combaseapi.h>
#include <unknwn.h>
#include <vector>
#include <list>
#include <cmath>
#include <functional>
#include <atomic>
#include <cstdint>

// Must be before WinRT headers — Windows SDK defines GetCurrentTime() as a
// function-like macro that conflicts with XAML's Media.Animation namespace
#undef GetCurrentTime

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/base.h>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "user32.lib")

using namespace winrt::Windows::UI::Xaml;

// ╔══════════════════════════════════════════════════════════╗
// ║                    CONFIGURATION                        ║
// ╚══════════════════════════════════════════════════════════╝
namespace Config {
    constexpr int kIconWidth      = 24;   // Tray icon size (Win11 default: 32)
    constexpr int kIconRows       = 1;    // 1=row, 2+=grid
    constexpr int kOverflowWidth  = 32;   // Overflow icon width
    constexpr int kOverflowPerRow = 5;    // Icons per row in overflow
    constexpr int kTimerMs        = 1500; // Fallback poll interval
}

// ╔══════════════════════════════════════════════════════════╗
// ║              PATTERN SCANNING                           ║
// ╚══════════════════════════════════════════════════════════╝
struct Pattern {
    std::vector<uint8_t> bytes, mask; // mask: 0xFF = wildcard
};

static Pattern MakePattern(const std::vector<uint8_t>& data, uint8_t wc = 0xAA) {
    Pattern p{data, std::vector<uint8_t>(data.size())};
    for (size_t i = 0; i < data.size(); i++) p.mask[i] = (data[i] == wc) ? 0xFF : 0x00;
    return p;
}

static bool Match(const uint8_t* d, const Pattern& p) {
    for (size_t i = 0; i < p.bytes.size(); i++) if (!p.mask[i] && d[i] != p.bytes[i]) return false;
    return true;
}

static uint8_t* Scan(const uint8_t* b, size_t sz, const Pattern& p) {
    if (sz < p.bytes.size()) return nullptr;
    for (size_t i = 0; i <= sz - p.bytes.size(); i++) if (Match(b + i, p)) return const_cast<uint8_t*>(b + i);
    return nullptr;
}

static uint8_t* ScanMod(HMODULE mod, const Pattern& p) {
    auto b = (const uint8_t*)mod;
    auto nt = (IMAGE_NT_HEADERS*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    return Scan(b, nt->OptionalHeader.SizeOfImage, p);
}

// ╔══════════════════════════════════════════════════════════╗
// ║              TASKBAR INTERNAL POINTERS                  ║
// ╚══════════════════════════════════════════════════════════╝
using GetTaskbarHostFn = void*(WINAPI*)(void*, void**);
using DecrefFn = void(WINAPI*)(void*);

static HMODULE          g_taskbar       = nullptr;
static GetTaskbarHostFn g_getTH         = nullptr;
static void*            g_frameH        = nullptr;
static DecrefFn         g_decref        = nullptr;
static void*            g_vtab         = nullptr;

static bool InitTaskbar() {
    if (g_getTH) return true;
    g_taskbar = GetModuleHandleW(L"taskbar.dll");
    if (!g_taskbar) g_taskbar = LoadLibraryExW(L"taskbar.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_taskbar) return false;

    auto* base = (const uint8_t*)g_taskbar;
    auto* nt   = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto* sec  = IMAGE_FIRST_SECTION(nt);

    const uint8_t* textS = nullptr; size_t textSz = 0;
    const uint8_t* rdataS = nullptr; size_t rdataSz = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (memcmp(sec[i].Name, ".text", 5) == 0)  { textS = base + sec[i].VirtualAddress; textSz = sec[i].SizeOfRawData; }
        if (memcmp(sec[i].Name, ".rdata", 6) == 0) { rdataS = base + sec[i].VirtualAddress; rdataSz = sec[i].SizeOfRawData; }
    }
    if (!textS) return false;

    // TaskbarHost::FrameHeight — sub rsp,28h; add rcx,XX; ...
    auto* fh = Scan(textS, textSz, MakePattern({0x48,0x83,0xEC,0x28,0x48,0x83,0xC1,0xAA}));
    if (!fh) fh = Scan(textS, textSz, MakePattern({0x48,0x83,0xEC,0x28,0x48,0x8D,0x41,0xAA}));
    g_frameH = fh;
    size_t elemOff = fh ? fh[7] : 0x48;

    // GetTaskbarHost
    for (auto& pat : {
        MakePattern({0x48,0x89,0x5C,0x24,0xAA,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9}),
        MakePattern({0x48,0x89,0x5C,0x24,0xAA,0x48,0x89,0x74,0x24,0xAA,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9}),
    }) {
        auto* f = Scan(textS, textSz, pat); if (!f) continue;
        for (int j = 0; j < 80 && f + j < textS + textSz - 3; j++) {
            if (f[j]==0x48 && f[j+1]==0x8D && (f[j+2]==0x4B||f[j+2]==0x4F) && f[j+3]==elemOff) { g_getTH = (GetTaskbarHostFn)f; break; }
            if (f[j]==0x48 && f[j+1]==0x8D && f[j+2]==0x41 && f[j+3]==elemOff) { g_getTH = (GetTaskbarHostFn)f; break; }
        }
        if (g_getTH) break;
    }

    // _Ref_count_base::_Decref — lock; dec [rcx+4]; ...
    auto* dec = Scan(textS, textSz, MakePattern({0xF0,0xFF,0x41,0x04}));
    if (!dec) dec = Scan(textS, textSz, MakePattern({0xF0,0xFF,0x49,0x04}));
    if (dec) {
        auto* p = dec;
        for (int i = 0; i < 80 && p > textS; p--) {
            i++;
            if ((p[0]==0x48 && p[1]==0x89 && p[2]==0x5C) || (p[0]==0x48 && p[1]==0x83 && p[2]==0xEC && p[3]<=0x40 && p>=textS)) { g_decref = (DecrefFn)p; break; }
        }
        if (!g_decref) g_decref = (DecrefFn)dec;
    }

    // ITaskListWndSite vtable
    if (g_getTH && rdataS) {
        auto fnAddr = (const uint8_t*)g_getTH;
        for (size_t i = 0; i < rdataSz - 8; i++) {
            if (memcmp(rdataS + i, &fnAddr, 8) == 0) {
                auto* entry = const_cast<uint8_t*>(rdataS + i);
                for (int b = 0; b < 25; b++) {
                    auto* cand = entry - (b + 1) * 8;
                    if (cand < rdataS) break;
                    if (*(uint8_t**)(cand) >= base && *(uint8_t**)(cand) < base + nt->OptionalHeader.SizeOfImage) { g_vtab = cand + 8; b = 25; }
                }
                break;
            }
        }
    }
    return g_getTH != nullptr;
}

// ╔══════════════════════════════════════════════════════════╗
// ║              XAML HELPERS                               ║
// ╚══════════════════════════════════════════════════════════╝

static HWND FindTrayWnd() {
    HWND r = nullptr;
    EnumWindows([](HWND h, LPARAM l) -> BOOL { DWORD pid; WCHAR c[32]; return (GetWindowThreadProcessId(h,&pid) && pid==GetCurrentProcessId() && GetClassNameW(h,c,32) && _wcsicmp(c,L"Shell_TrayWnd")==0) ? (*(HWND*)l=h,FALSE) : TRUE; }, (LPARAM)&r);
    return r;
}

static FrameworkElement EnumChildren(FrameworkElement p, std::function<bool(FrameworkElement)> cb) {
    int n = Media::VisualTreeHelper::GetChildrenCount(p);
    for (int i = 0; i < n; i++) if (auto c = Media::VisualTreeHelper::GetChild(p,i).try_as<FrameworkElement>()) if (cb(c)) return c;
    return nullptr;
}

static FrameworkElement FindName(FrameworkElement e, PCWSTR n) { return EnumChildren(e, [&](auto c) { return c.Name() == n; }); }
static FrameworkElement FindClass(FrameworkElement e, PCWSTR c) { return EnumChildren(e, [&](auto c2) { return winrt::get_class_name(c2) == c; }); }

// ╔══════════════════════════════════════════════════════════╗
// ║              STYLE APPLICATION                           ║
// ╚══════════════════════════════════════════════════════════╝

static void StyleIconView(FrameworkElement e, int w) {
    e.MinWidth(w);
    auto c = e;
    if ((c=FindName(c,L"ContainerGrid")) && (c=FindName(c,L"ContentPresenter")) && (c=FindName(c,L"ContentGrid")))
        EnumChildren(c, [w](auto ch) {
            auto cls = winrt::get_class_name(ch);
            if (cls==L"SystemTray.TextIconContent" || cls==L"SystemTray.ImageIconContent") { if (auto g = FindName(ch,L"ContainerGrid").try_as<Controls::Grid>()) g.Padding(Thickness{}); }
            else if (cls == L"SystemTray.LanguageTextIconContent") { ch.Width(std::numeric_limits<double>::quiet_NaN()); ch.MinWidth(w + 12); }
            return false;
        });
}

static void StyleOverFlowIcon(FrameworkElement e, int w) {
    e.MinWidth(w); e.Height(w);
    auto c = e;
    if ((c=FindName(c,L"ContainerGrid")) && (c=FindName(c,L"ContentPresenter")) && (c=FindName(c,L"ContentGrid")))
        EnumChildren(c, [](auto ch) { if (winrt::get_class_name(ch)==L"SystemTray.ImageIconContent") if (auto g=FindName(ch,L"ContainerGrid").try_as<Controls::Grid>()) g.Padding(Thickness{}); return false; });
}

static void StyleTrayIcon(FrameworkElement e, int w) {
    auto c = e;
    if ((c=FindName(c,L"ContainerGrid")) && (c=FindName(c,L"ContentGrid")) && (c=FindClass(c,L"SystemTray.TextIconContent")) && (c=FindName(c,L"ContainerGrid")))
        if (auto g = c.try_as<Controls::Grid>()) { int p = (w>32)?(8+w-32)/2:(w<24)?std::max((8+w-24)/2,0):4; g.Padding(Thickness{(double)p,0,(double)p,0}); }
}

static void StyleGrid(FrameworkElement sp, int rows, int w) {
    double ih = 0;
    if (rows > 1) { double g = std::fmax(sp.ActualHeight() - 16 * rows, 0.0); ih = 16 + (static_cast<int>(g / (rows + 1)) / 2 * 2); }
    int n = Media::VisualTreeHelper::GetChildrenCount(sp), cols = (n + rows - 1) / rows, idx = 0;
    EnumChildren(sp, [&](auto c) {
        int i = idx++;
        if (rows > 1) { c.Height(ih); Media::TranslateTransform t; t.X(w * ((i%cols)-i)); t.Y(ih*(i/cols)-ih*(rows-1)/2); c.RenderTransform(t); }
        else { auto d=c.as<DependencyObject>(); d.ClearValue(FrameworkElement::HeightProperty()); d.ClearValue(UIElement::RenderTransformProperty()); }
        return false;
    });
    if (rows > 1) sp.Width(w * ((idx + rows - 1) / rows)); else sp.as<DependencyObject>().ClearValue(FrameworkElement::WidthProperty());
}

static void StyleNotifyArea(FrameworkElement area, int rows, int w) {
    FrameworkElement sp = nullptr, c = area;
    if ((c=FindClass(c,L"Windows.UI.Xaml.Controls.ItemsPresenter")) && (c=FindClass(c,L"Windows.UI.Xaml.Controls.StackPanel"))) sp = c;
    if (!sp) return;
    EnumChildren(sp, [w](auto c) { if (winrt::get_class_name(c)!=L"Windows.UI.Xaml.Controls.ContentPresenter") return false; if (auto i=FindName(c,L"NotifyItemIcon")) StyleIconView(i,w); return false; });
    StyleGrid(sp, rows, w);
}

static bool StyleCtrlCenter(FrameworkElement btn, int w) {
    FrameworkElement sp = nullptr, c = btn;
    if ((c=FindClass(c,L"Windows.UI.Xaml.Controls.Grid")) && (c=FindName(c,L"ContentPresenter")) && (c=FindClass(c,L"Windows.UI.Xaml.Controls.ItemsPresenter")) && (c=FindClass(c,L"Windows.UI.Xaml.Controls.StackPanel"))) sp = c;
    if (!sp) return false;
    EnumChildren(sp, [w](auto c) { if (winrt::get_class_name(c)!=L"Windows.UI.Xaml.Controls.ContentPresenter") return false; if (auto i=FindName(c,L"SystemTrayIcon")) StyleTrayIcon(i,w); return false; });
    return true;
}

static bool StyleStack(PCWSTR name, FrameworkElement container, int w) {
    FrameworkElement sp = nullptr, c = container;
    if ((c=FindName(c,L"Content")) && (c=FindName(c,L"IconStack")) && (c=FindClass(c,L"Windows.UI.Xaml.Controls.ItemsPresenter")) && (c=FindClass(c,L"Windows.UI.Xaml.Controls.StackPanel"))) sp = c;
    if (!sp) return false;
    EnumChildren(sp, [name,w](auto c) {
        if (winrt::get_class_name(c)!=L"Windows.UI.Xaml.Controls.ContentPresenter") return false;
        if (wcscmp(name,L"NotifyIconStack")==0) { if (auto ch=FindClass(c,L"SystemTray.ChevronIconView")) StyleIconView(ch,w); }
        else { if (auto i=FindName(c,L"SystemTrayIcon")) StyleIconView(i,w); }
        return false;
    });
    return true;
}

static void StyleOverflow(FrameworkElement root) {
    Controls::WrapGrid wg = nullptr; auto c = root;
    if ((c=FindClass(c,L"Windows.UI.Xaml.Controls.ItemsControl")) && (c=FindClass(c,L"Windows.UI.Xaml.Controls.ItemsPresenter")) && (c=FindClass(c,L"Windows.UI.Xaml.Controls.WrapGrid"))) wg = c.try_as<Controls::WrapGrid>();
    if (!wg) return;
    wg.ItemWidth(Config::kOverflowWidth); wg.ItemHeight(Config::kOverflowWidth); wg.MaximumRowsOrColumns(Config::kOverflowPerRow);
    EnumChildren(wg, [](auto c) { if (winrt::get_class_name(c)!=L"Windows.UI.Xaml.Controls.ContentPresenter") return false; if (auto nv=FindClass(c,L"SystemTray.NotifyIconView")) StyleOverFlowIcon(nv,Config::kOverflowWidth); return false; });
}

// ╔══════════════════════════════════════════════════════════╗
// ║              XAML ROOT                                  ║
// ╚══════════════════════════════════════════════════════════╝

static XamlRoot GetRoot(HWND hTray) {
    if (!InitTaskbar()) return nullptr;
    HWND hSw = (HWND)GetPropW(hTray, L"TaskbandHWND");
    if (!hSw) return nullptr;
    auto* tb = (void*)GetWindowLongPtrW(hSw, 0);
    if (!tb) return nullptr;

    void* itf = tb; bool found = false;
    if (g_vtab) { for (int i=0;i<25;i++) { if (*(void**)((void**)tb+i)==g_vtab) { itf=(void**)tb+i; found=true; break; } } }
    if (!found && g_getTH) { for (int i=0;i<25;i++) { auto* vt=*(void***)((void**)tb+i); if (!vt||(uint8_t*)vt<(uint8_t*)g_taskbar||(uint8_t*)vt>(uint8_t*)g_taskbar+0x200000) continue; for (int j=0;j<30;j++) { if (vt[j]==(void*)g_getTH) { itf=(void**)tb+i; found=true; break; } } if (found) break; } }
    if (!found) return nullptr;

    void* sp[2] = {}; g_getTH(itf, sp);
    if (!sp[0]) return nullptr;
    size_t off = g_frameH ? ((uint8_t*)g_frameH)[7] : 0x48;
    auto* unk = *(IUnknown**)((BYTE*)sp[0] + off);
    if (!unk) { if (g_decref) g_decref(sp[1]); return nullptr; }
    FrameworkElement te = nullptr; unk->QueryInterface(winrt::guid_of<FrameworkElement>(), winrt::put_abi(te));
    XamlRoot r = te ? te.XamlRoot() : nullptr;
    if (g_decref) g_decref(sp[1]);
    return r;
}

// ╔══════════════════════════════════════════════════════════╗
// ║              APPLY ALL STYLES                           ║
// ╚══════════════════════════════════════════════════════════╝

void ApplyStyles(int w, int rows) {
    auto h = FindTrayWnd();
    if (!h) return;
    auto rt = GetRoot(h);
    if (!rt) return;
    auto co = rt.Content().try_as<FrameworkElement>();
    if (!co) return;
    auto f = FindClass(co, L"SystemTray.SystemTrayFrame");
    if (!f) return;
    auto g = FindName(f, L"SystemTrayFrameGrid");
    if (!g) return;

    if (auto a = FindName(g, L"NotificationAreaIcons")) StyleNotifyArea(a, rows, w);
    if (auto b = FindName(g, L"ControlCenterButton")) StyleCtrlCenter(b, w);
    for (auto n : {L"NotifyIconStack", L"MainStack", L"NonActivatableStack"}) if (auto c = FindName(g, n)) StyleStack(n, c, w);
    if (auto ov = FindClass(g, L"Windows.UI.Xaml.Controls.Grid")) StyleOverflow(ov);
}

// ╔══════════════════════════════════════════════════════════╗
// ║              WinEventHook for instant response          ║
// ╚══════════════════════════════════════════════════════════╝
// Instead of function-level hooks (which need a detour library),
// we listen for UI automation events. When the tray area creates
// new child elements, we reapply styles immediately.

static HWINEVENTHOOK g_eventHook = nullptr;

static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                   LONG idObject, LONG idChild,
                                   DWORD dwEventThread, DWORD dwmsEventTime) {
    if (g_unloading) return;

    // Filter for OBJECT_CREATE/SHOW events within explorer.exe
    if (event == EVENT_OBJECT_CREATE || event == EVENT_OBJECT_SHOW) {
        // Check if this window is related to the taskbar tray
        if (idObject == OBJID_WINDOW && hwnd) {
            WCHAR cls[32];
            if (GetClassNameW(hwnd, cls, 32)) {
                // Tray-related windows
                if (wcscmp(cls, L"NotifyIconOverflowWindow") == 0 ||
                    wcscmp(cls, L"Windows.UI.Composition.DesktopWindowContentBridge") == 0) {
                    ApplyStyles(Config::kIconWidth, Config::kIconRows);
                }
            }
        }
    }
}

// ╔══════════════════════════════════════════════════════════╗
// ║              THREAD + TIMER                             ║
// ╚══════════════════════════════════════════════════════════╝

static std::atomic<bool> g_unloading{false};
static UINT_PTR g_timerId = 0;

static VOID CALLBACK OnTimer(HWND, UINT, UINT_PTR id, DWORD) {
    if (g_unloading) { KillTimer(nullptr, id); return; }
    ApplyStyles(Config::kIconWidth, Config::kIconRows);
}

static DWORD WINAPI WorkerThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Apply immediately on injection
    ApplyStyles(Config::kIconWidth, Config::kIconRows);

    // Set up WinEventHook for near-instant response to new tray elements
    g_eventHook = SetWinEventHook(
        EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW,
        nullptr, WinEventProc,
        GetCurrentProcessId(), 0, // only events from our process (explorer.exe)
        WINEVENT_OUTOFCONTEXT);

    // Fallback timer (catches any changes the event hook misses)
    g_timerId = SetTimer(nullptr, 0, Config::kTimerMs, OnTimer);

    MSG msg;
    while (!g_unloading && GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_eventHook) UnhookWinEvent(g_eventHook);
    if (g_timerId) KillTimer(nullptr, g_timerId);
    CoUninitialize();
    return 0;
}

// ╔══════════════════════════════════════════════════════════╗
// ║              DLL MAIN                                   ║
// ╚══════════════════════════════════════════════════════════╝

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hMod);
        if (auto h = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr)) CloseHandle(h);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_unloading = true;
        if (g_eventHook) UnhookWinEvent(g_eventHook);
        if (g_timerId) KillTimer(nullptr, g_timerId);
        if (g_taskbar) FreeLibrary(g_taskbar);
    }
    return TRUE;
}
