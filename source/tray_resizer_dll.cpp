// ============================================================
// tray_resizer_dll.cpp — Standalone Taskbar Tray Icon Resizer
// ============================================================
// Extracted from the Windhawk "Taskbar tray icon spacing and grid" mod
// by m417z. This is the worker DLL that gets injected into explorer.exe.
//
// HOW TO BUILD (x64 Native Tools Command Prompt for VS 2022+):
//   cl /EHsc /MD /LD /std:c++20 /Zc:preprocessor tray_resizer_dll.cpp ^
//       /Fe:tray_resizer_dll.dll ^
//       /I"%WindowsSdkDir%Include\%WindowsSDKVersion%\cppwinrt" ^
//       /link windowsapp.lib ole32.lib oleaut32.lib runtimeobject.lib
//
// If that fails, try the batch file: build.bat
// ============================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <psapi.h>
#include <combaseapi.h>  // CoInitializeEx, CoUninitialize
#include <unknwn.h>      // IUnknown
#include <vector>
#include <list>
#include <cmath>
#include <functional>
#include <atomic>
#include <cstdint>

// Fix macro conflict between Windows SDK and WinRT XAML headers
// Must be before WinRT includes!
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

using namespace winrt::Windows::UI::Xaml;

// ╔══════════════════════════════════════════════════════════╗
// ║                    CONFIGURATION                        ║
// ║  Change these values and recompile to customize         ║
// ╚══════════════════════════════════════════════════════════╝
namespace Config {
    constexpr int kIconWidth           = 24;   // Tray icon size (Win11 default: 32)
    constexpr int kIconRows            = 1;    // 1=row, 2+=grid
    constexpr int kOverflowWidth       = 32;   // Overflow icon width
    constexpr int kOverflowPerRow      = 5;    // Icons per row in overflow
    constexpr int kReapplyMs           = 4000; // Recheck interval
}

// ╔══════════════════════════════════════════════════════════╗
// ║              PATTERN SCANNING UTILITIES                 ║
// ╚══════════════════════════════════════════════════════════╝
struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> mask; // 0xFF = wildcard
};

static Pattern MakePattern(const std::vector<uint8_t>& data, uint8_t wildcard = 0xAA) {
    Pattern p{data, std::vector<uint8_t>(data.size())};
    for (size_t i = 0; i < data.size(); i++)
        p.mask[i] = (data[i] == wildcard) ? 0xFF : 0x00;
    return p;
}

static bool Match(const uint8_t* data, const Pattern& p) {
    for (size_t i = 0; i < p.bytes.size(); i++)
        if (!p.mask[i] && data[i] != p.bytes[i]) return false;
    return true;
}

static uint8_t* Scan(const uint8_t* base, size_t size, const Pattern& p) {
    if (size < p.bytes.size()) return nullptr;
    for (size_t i = 0; i <= size - p.bytes.size(); i++)
        if (Match(base + i, p)) return const_cast<uint8_t*>(base + i);
    return nullptr;
}

static uint8_t* ScanModule(HMODULE mod, const Pattern& p) {
    auto* base = (const uint8_t*)mod;
    auto* dos  = (IMAGE_DOS_HEADER*)base;
    auto* nt   = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    return Scan(base, nt->OptionalHeader.SizeOfImage, p);
}

// ╔══════════════════════════════════════════════════════════╗
// ║              TASKBAR INTERNAL POINTERS                  ║
// ╚══════════════════════════════════════════════════════════╝

using GetTaskbarHostFn  = void*(WINAPI*)(void* pThis, void** out);
using DecrefFn          = void(WINAPI*)(void* pThis);

static HMODULE          g_taskbarMod        = nullptr;
static GetTaskbarHostFn g_getTaskbarHost    = nullptr;
static void*            g_frameHeight       = nullptr;
static DecrefFn         g_decref            = nullptr;
static void*            g_vtable_ITaskList  = nullptr;

static bool InitTaskbarPointers() {
    if (g_getTaskbarHost) return true;

    g_taskbarMod = GetModuleHandleW(L"taskbar.dll");
    if (!g_taskbarMod)
        g_taskbarMod = LoadLibraryExW(L"taskbar.dll", nullptr,
                                     LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_taskbarMod) return false;

    auto* base = (const uint8_t*)(g_taskbarMod);
    auto* dos  = (IMAGE_DOS_HEADER*)base;
    auto* nt   = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);

    // Locate .text section
    const uint8_t* textStart = nullptr;
    size_t textSize = 0;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (memcmp(sec[i].Name, ".text", 5) == 0) {
            textStart = base + sec[i].VirtualAddress;
            textSize  = sec[i].SizeOfRawData;
            break;
        }
    }
    if (!textStart) return false;

    // ── Step 1: Find TaskbarHost::FrameHeight ──
    // Pattern: sub rsp,28h; add rcx,XX; ...
    Pattern fh1 = MakePattern({0x48,0x83,0xEC,0x28,0x48,0x83,0xC1,0xAA});
    Pattern fh2 = MakePattern({0x48,0x83,0xEC,0x28,0x48,0x8D,0x41,0xAA});
    Pattern fh4 = MakePattern({0x48,0x83,0xEC,0x38,0x48,0x83,0xC1,0xAA});

    auto* fh = Scan(textStart, textSize, fh1);
    if (!fh) fh = Scan(textStart, textSize, fh2);
    if (!fh) fh = Scan(textStart, textSize, fh4);
    g_frameHeight = fh;

    size_t elementOffset = 0x48; // default fallback
    if (fh) elementOffset = fh[7];

    // ── Step 2: Find CTaskBand::GetTaskbarHost ──
    std::vector<Pattern> patterns = {
        MakePattern({0x48,0x89,0x5C,0x24,0xAA, 0x57, 0x48,0x83,0xEC,0x20, 0x48,0x8B,0xD9}),
        MakePattern({0x48,0x89,0x5C,0x24,0xAA, 0x48,0x89,0x74,0x24,0xAA, 0x57, 0x48,0x83,0xEC,0x20, 0x48,0x8B,0xD9}),
        MakePattern({0x48,0x89,0x5C,0x24,0xAA, 0x48,0x89,0x6C,0x24,0xAA, 0x48,0x89,0x74,0x24,0xAA, 0x57, 0x48,0x83,0xEC,0x20, 0x48,0x8B,0xD9}),
        MakePattern({0x48,0x89,0x5C,0x24,0xAA, 0x48,0x89,0x74,0x24,0xAA, 0x57, 0x48,0x83,0xEC,0x20, 0x48,0x8B,0xF9}),
    };

    for (auto& pat : patterns) {
        auto* found = Scan(textStart, textSize, pat);
        if (!found) continue;
        bool hasRef = false;
        for (int j = 0; j < 80 && (found + j) < (textStart + textSize - 3); j++) {
            if (found[j] == 0x48 && found[j+1] == 0x8D &&
                (found[j+2] == 0x4B || found[j+2] == 0x4F) &&
                found[j+3] == elementOffset) { hasRef = true; break; }
            if (found[j] == 0x48 && found[j+1] == 0x8D && found[j+2] == 0x41 &&
                found[j+3] == elementOffset) { hasRef = true; break; }
        }
        if (hasRef) { g_getTaskbarHost = (GetTaskbarHostFn)found; break; }
    }

    // ── Step 3: Find std::_Ref_count_base::_Decref ──
    Pattern dec1 = MakePattern({0xF0,0xFF,0x41,0x04});
    Pattern dec2 = MakePattern({0xF0,0xFF,0x49,0x04});
    auto* decref = Scan(textStart, textSize, dec1);
    if (!decref) decref = Scan(textStart, textSize, dec2);
    if (decref) {
        auto* func = decref;
        for (int i = 0; i < 80 && func > textStart; i--) {
            func--;
            if (func[0] == 0x48 && func[1] == 0x89 && func[2] == 0x5C && func[3] == 0x24) {
                g_decref = (DecrefFn)func; break;
            }
            if (func[0] == 0x48 && func[1] == 0x83 && func[2] == 0xEC && func[3] <= 0x40) {
                if (func >= textStart) { g_decref = (DecrefFn)func; break; }
            }
        }
        if (!g_decref) g_decref = (DecrefFn)decref;
    }

    // ── Step 4: Find ITaskListWndSite vtable ──
    if (g_getTaskbarHost) {
        auto fnAddr = (const uint8_t*)g_getTaskbarHost;
        sec = IMAGE_FIRST_SECTION(nt);
        const uint8_t* rdataStart = nullptr;
        size_t rdataSize = 0;
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
            if (memcmp(sec[i].Name, ".rdata", 6) == 0) {
                rdataStart = base + sec[i].VirtualAddress;
                rdataSize  = sec[i].SizeOfRawData;
                break;
            }
        }
        if (rdataStart) {
            for (size_t i = 0; i < rdataSize - 8; i++) {
                if (memcmp(rdataStart + i, &fnAddr, 8) == 0) {
                    uint8_t* entry = const_cast<uint8_t*>(rdataStart + i);
                    for (int back = 0; back < 25; back++) {
                        auto* cand = entry - (back + 1) * 8;
                        if (cand < rdataStart) break;
                        auto* rttiPtr = *(uint8_t**)(cand);
                        if (rttiPtr >= base && rttiPtr < base + nt->OptionalHeader.SizeOfImage) {
                            g_vtable_ITaskList = cand + 8; break;
                        }
                    }
                    break;
                }
            }
        }
    }

    return g_getTaskbarHost != nullptr;
}

// ╔══════════════════════════════════════════════════════════╗
// ║              XAML HELPER & STYLE FUNCTIONS              ║
// ╚══════════════════════════════════════════════════════════╝

static HWND FindTaskbarWnd() {
    HWND result = nullptr;
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        DWORD pid; WCHAR cls[32];
        if (GetWindowThreadProcessId(hwnd, &pid) && pid == GetCurrentProcessId() &&
            GetClassNameW(hwnd, cls, 32) && _wcsicmp(cls, L"Shell_TrayWnd") == 0) {
            *(HWND*)lp = hwnd; return FALSE;
        }
        return TRUE;
    }, (LPARAM)&result);
    return result;
}

static FrameworkElement EnumChildren(FrameworkElement parent,
                                      std::function<bool(FrameworkElement)> cb) {
    int n = Media::VisualTreeHelper::GetChildrenCount(parent);
    for (int i = 0; i < n; i++) {
        if (auto c = Media::VisualTreeHelper::GetChild(parent, i).try_as<FrameworkElement>())
            if (cb(c)) return c;
    }
    return nullptr;
}

static FrameworkElement FindChildByName(FrameworkElement el, PCWSTR name) {
    return EnumChildren(el, [&](auto c) { return c.Name() == name; });
}

static FrameworkElement FindChildByClass(FrameworkElement el, PCWSTR cls) {
    return EnumChildren(el, [&](auto c) { return winrt::get_class_name(c) == cls; });
}

static void StyleNotifyIconView(FrameworkElement el, int w) {
    el.MinWidth(w);
    auto c = el;
    if ((c = FindChildByName(c, L"ContainerGrid")) &&
        (c = FindChildByName(c, L"ContentPresenter")) &&
        (c = FindChildByName(c, L"ContentGrid"))) {
        EnumChildren(c, [w](auto ch) {
            auto cls = winrt::get_class_name(ch);
            if (cls == L"SystemTray.TextIconContent" || cls == L"SystemTray.ImageIconContent") {
                if (auto g = FindChildByName(ch, L"ContainerGrid").try_as<Controls::Grid>())
                    g.Padding(Thickness{});
            } else if (cls == L"SystemTray.LanguageTextIconContent") {
                ch.Width(std::numeric_limits<double>::quiet_NaN());
                ch.MinWidth(w + 12);
            }
            return false;
        });
    }
}

static void StyleOverflowNotifyIcon(FrameworkElement el, int w) {
    el.MinWidth(w);
    el.Height(w);
    auto c = el;
    if ((c = FindChildByName(c, L"ContainerGrid")) &&
        (c = FindChildByName(c, L"ContentPresenter")) &&
        (c = FindChildByName(c, L"ContentGrid"))) {
        EnumChildren(c, [](auto ch) {
            if (winrt::get_class_name(ch) == L"SystemTray.ImageIconContent")
                if (auto g = FindChildByName(ch, L"ContainerGrid").try_as<Controls::Grid>())
                    g.Padding(Thickness{});
            return false;
        });
    }
}

static void StyleSystemTrayIcon(FrameworkElement el, int w) {
    auto c = el;
    if ((c = FindChildByName(c, L"ContainerGrid")) &&
        (c = FindChildByName(c, L"ContentGrid")) &&
        (c = FindChildByClass(c, L"SystemTray.TextIconContent")) &&
        (c = FindChildByName(c, L"ContainerGrid"))) {
        if (auto g = c.try_as<Controls::Grid>()) {
            int pad = (w > 32) ? ((8 + w - 32) / 2) : (w < 24) ? std::max((8 + w - 24) / 2, 0) : 4;
            g.Padding(Thickness{(double)pad, 0, (double)pad, 0});
        }
    }
}

static void StyleStackPanelGrid(FrameworkElement sp, int rows, int w) {
    double ih = 0;
    if (rows > 1) {
        double gap = std::fmax(sp.ActualHeight() - 16 * rows, 0.0);
        ih = 16 + (static_cast<int>(gap / (rows + 1)) / 2 * 2);
    }
    int n = Media::VisualTreeHelper::GetChildrenCount(sp);
    int cols = (n + rows - 1) / rows;
    int idx = 0;

    EnumChildren(sp, [&](auto c) {
        int i = idx++;
        if (rows > 1) {
            c.Height(ih);
            Media::TranslateTransform t;
            t.X(w * ((i % cols) - i));
            t.Y(ih * (i / cols) - ih * (rows - 1) / 2);
            c.RenderTransform(t);
        } else {
            auto dp = c.as<DependencyObject>();
            dp.ClearValue(FrameworkElement::HeightProperty());
            dp.ClearValue(UIElement::RenderTransformProperty());
        }
        return false;
    });

    if (rows > 1) sp.Width(w * ((idx + rows - 1) / rows));
    else sp.as<DependencyObject>().ClearValue(FrameworkElement::WidthProperty());
}

static void StyleNotifyArea(FrameworkElement area, int rows, int w) {
    FrameworkElement sp = nullptr, c = area;
    if ((c = FindChildByClass(c, L"Windows.UI.Xaml.Controls.ItemsPresenter")) &&
        (c = FindChildByClass(c, L"Windows.UI.Xaml.Controls.StackPanel")))
        sp = c;
    if (!sp) return;

    EnumChildren(sp, [w](auto c) {
        if (winrt::get_class_name(c) != L"Windows.UI.Xaml.Controls.ContentPresenter")
            return false;
        if (auto icon = FindChildByName(c, L"NotifyItemIcon"))
            StyleNotifyIconView(icon, w);
        return false;
    });
    StyleStackPanelGrid(sp, rows, w);
}

static bool StyleControlCenter(FrameworkElement btn, int w) {
    FrameworkElement sp = nullptr, c = btn;
    if ((c = FindChildByClass(c, L"Windows.UI.Xaml.Controls.Grid")) &&
        (c = FindChildByName(c, L"ContentPresenter")) &&
        (c = FindChildByClass(c, L"Windows.UI.Xaml.Controls.ItemsPresenter")) &&
        (c = FindChildByClass(c, L"Windows.UI.Xaml.Controls.StackPanel")))
        sp = c;
    if (!sp) return false;
    EnumChildren(sp, [w](auto c) {
        if (winrt::get_class_name(c) != L"Windows.UI.Xaml.Controls.ContentPresenter")
            return false;
        if (auto icon = FindChildByName(c, L"SystemTrayIcon"))
            StyleSystemTrayIcon(icon, w);
        return false;
    });
    return true;
}

static bool StyleIconStack(PCWSTR name, FrameworkElement container, int w) {
    FrameworkElement sp = nullptr, c = container;
    if ((c = FindChildByName(c, L"Content")) &&
        (c = FindChildByName(c, L"IconStack")) &&
        (c = FindChildByClass(c, L"Windows.UI.Xaml.Controls.ItemsPresenter")) &&
        (c = FindChildByClass(c, L"Windows.UI.Xaml.Controls.StackPanel")))
        sp = c;
    if (!sp) return false;

    EnumChildren(sp, [name, w](auto c) {
        if (winrt::get_class_name(c) != L"Windows.UI.Xaml.Controls.ContentPresenter")
            return false;
        if (wcscmp(name, L"NotifyIconStack") == 0) {
            if (auto ch = FindChildByClass(c, L"SystemTray.ChevronIconView"))
                StyleNotifyIconView(ch, w);
        } else {
            if (auto icon = FindChildByName(c, L"SystemTrayIcon"))
                StyleNotifyIconView(icon, w);
        }
        return false;
    });
    return true;
}

static void StyleOverflow(FrameworkElement root) {
    Controls::WrapGrid wg = nullptr;
    auto c = root;
    if ((c = FindChildByClass(c, L"Windows.UI.Xaml.Controls.ItemsControl")) &&
        (c = FindChildByClass(c, L"Windows.UI.Xaml.Controls.ItemsPresenter")) &&
        (c = FindChildByClass(c, L"Windows.UI.Xaml.Controls.WrapGrid")))
        wg = c.try_as<Controls::WrapGrid>();
    if (!wg) return;

    int w = Config::kOverflowWidth;
    wg.ItemWidth(w);
    wg.ItemHeight(w);
    wg.MaximumRowsOrColumns(Config::kOverflowPerRow);

    EnumChildren(wg, [w](auto c) {
        if (winrt::get_class_name(c) != L"Windows.UI.Xaml.Controls.ContentPresenter")
            return false;
        if (auto nv = FindChildByClass(c, L"SystemTray.NotifyIconView"))
            StyleOverflowNotifyIcon(nv, w);
        return false;
    });
}

// ╔══════════════════════════════════════════════════════════╗
// ║              XAML ROOT ACQUISITION                      ║
// ╚══════════════════════════════════════════════════════════╝

static XamlRoot GetXamlRoot(HWND hTaskbar) {
    if (!InitTaskbarPointers()) return nullptr;

    HWND hSwWnd = (HWND)GetPropW(hTaskbar, L"TaskbandHWND");
    if (!hSwWnd) return nullptr;

    auto* taskBand = (void*)GetWindowLongPtrW(hSwWnd, 0);
    if (!taskBand) return nullptr;

    void* itfPtr = taskBand;
    bool found = false;

    if (g_vtable_ITaskList) {
        for (int i = 0; i < 25; i++) {
            if (*(void**)((void**)taskBand + i) == g_vtable_ITaskList) {
                itfPtr = (void**)taskBand + i;
                found = true;
                break;
            }
        }
    }

    if (!found && g_getTaskbarHost) {
        for (int i = 0; i < 25; i++) {
            auto* vtable = *(void***)((void**)taskBand + i);
            if (!vtable) continue;
            auto* b = (const uint8_t*)g_taskbarMod;
            if ((uint8_t*)vtable < b || (uint8_t*)vtable > b + 0x200000) continue;
            for (int j = 0; j < 30; j++) {
                if (vtable[j] == (void*)g_getTaskbarHost) {
                    itfPtr = (void**)taskBand + i;
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
    }

    if (!found) return nullptr;

    void* sp[2] = {};
    g_getTaskbarHost(itfPtr, sp);
    if (!sp[0]) return nullptr;

    size_t offset = 0x48;
    if (g_frameHeight) offset = ((uint8_t*)g_frameHeight)[7];

    auto* elementUnk = *(IUnknown**)((BYTE*)sp[0] + offset);
    if (!elementUnk) {
        if (g_decref) g_decref(sp[1]);
        return nullptr;
    }

    FrameworkElement taskbarElement = nullptr;
    elementUnk->QueryInterface(winrt::guid_of<FrameworkElement>(),
                                winrt::put_abi(taskbarElement));

    XamlRoot result = nullptr;
    if (taskbarElement) result = taskbarElement.XamlRoot();

    if (g_decref) g_decref(sp[1]);
    return result;
}

// ╔══════════════════════════════════════════════════════════╗
// ║              MAIN WORK LOGIC                            ║
// ╚══════════════════════════════════════════════════════════╝

static void ApplyStyles(int width, int rows) {
    auto hTaskbar = FindTaskbarWnd();
    if (!hTaskbar) return;

    auto root = GetXamlRoot(hTaskbar);
    if (!root) return;

    auto content = root.Content().try_as<FrameworkElement>();
    if (!content) return;

    auto frame = FindChildByClass(content, L"SystemTray.SystemTrayFrame");
    if (!frame) return;
    auto grid = FindChildByName(frame, L"SystemTrayFrameGrid");
    if (!grid) return;

    if (auto area = FindChildByName(grid, L"NotificationAreaIcons"))
        StyleNotifyArea(area, rows, width);

    if (auto btn = FindChildByName(grid, L"ControlCenterButton"))
        StyleControlCenter(btn, width);

    for (auto name : {L"NotifyIconStack", L"MainStack", L"NonActivatableStack"})
        if (auto c = FindChildByName(grid, name))
            StyleIconStack(name, c, width);

    if (auto overflow = FindChildByClass(grid, L"Windows.UI.Xaml.Controls.Grid"))
        StyleOverflow(overflow);
}

// ╔══════════════════════════════════════════════════════════╗
// ║              BACKGROUND REAPPLICATION                   ║
// ╚══════════════════════════════════════════════════════════╝

static std::atomic<bool> g_unloading{false};
static UINT_PTR g_timerId = 0;

static VOID CALLBACK OnTimer(HWND, UINT, UINT_PTR id, DWORD) {
    if (g_unloading) { KillTimer(nullptr, id); return; }
    ApplyStyles(Config::kIconWidth, Config::kIconRows);
}

static DWORD WINAPI WorkerThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ApplyStyles(Config::kIconWidth, Config::kIconRows);

    g_timerId = SetTimer(nullptr, 0, Config::kReapplyMs, OnTimer);

    MSG msg;
    while (!g_unloading && GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_timerId) KillTimer(nullptr, g_timerId);
    CoUninitialize();
    return 0;
}

// ╔══════════════════════════════════════════════════════════╗
// ║              DLL ENTRY POINT                            ║
// ╚══════════════════════════════════════════════════════════╝

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hMod);
        if (auto h = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr))
            CloseHandle(h);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_unloading = true;
        if (g_timerId) KillTimer(nullptr, g_timerId);
        if (g_taskbarMod) FreeLibrary(g_taskbarMod);
    }
    return TRUE;
}
