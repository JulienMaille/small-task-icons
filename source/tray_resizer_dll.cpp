// tray_resizer_dll.cpp — MinHook-based approach + window property enumeration
// Fully rewritten: hooks CreateWindowExW, finds XamlRoot via bridge window properties

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <combaseapi.h>
#include <unknwn.h>
#include <cmath>
#include <functional>
#include <atomic>
#include <vector>
#include <shlwapi.h>

#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/base.h>

#include "MinHook.h"
#pragma comment(lib, "libMinHook.x64.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "user32.lib")

using namespace winrt::Windows::UI::Xaml;
constexpr int ICON_W=24, ICON_R=1, OV_W=32, OV_R=5;

static FILE* g_log = nullptr;
static void LOG(const wchar_t* fmt, ...) {
    va_list args; va_start(args, fmt);
    WCHAR buf[1024]; wvsprintfW(buf, fmt, args);
    va_end(args);
    OutputDebugStringW(buf);
}

// ╔══════════════════════════════════════════════════════════╗
// ║  Find XAML root via DesktopWindowContentBridge props    ║
// ╚══════════════════════════════════════════════════════════╝
static FrameworkElement EC(FrameworkElement p,auto cb){int n=Media::VisualTreeHelper::GetChildrenCount(p);for(int i=0;i<n;i++)if(auto c=Media::VisualTreeHelper::GetChild(p,i).try_as<FrameworkElement>())if(cb(c))return c;return nullptr;}
static FrameworkElement FN(FrameworkElement e,PCWSTR n){return EC(e,[&](auto c){return c.Name()==n;});}
static FrameworkElement FC(FrameworkElement e,PCWSTR c){return EC(e,[&](auto c2){return winrt::get_class_name(c2)==c;});}
static void SI(FrameworkElement e,int w){e.MinWidth(w);auto c=e;if((c=FN(c,L"ContainerGrid"))&&(c=FN(c,L"ContentPresenter"))&&(c=FN(c,L"ContentGrid")))EC(c,[w](auto ch){auto cls=winrt::get_class_name(ch);if(cls==L"SystemTray.TextIconContent"||cls==L"SystemTray.ImageIconContent"){if(auto g=FN(ch,L"ContainerGrid").try_as<Controls::Grid>())g.Padding(Thickness{});}else if(cls==L"SystemTray.LanguageTextIconContent"){ch.Width(std::numeric_limits<double>::quiet_NaN());ch.MinWidth(w+12);}return false;});}
static void SOI(FrameworkElement e,int w){e.MinWidth(w);e.Height(w);auto c=e;if((c=FN(c,L"ContainerGrid"))&&(c=FN(c,L"ContentPresenter"))&&(c=FN(c,L"ContentGrid")))EC(c,[](auto ch){if(winrt::get_class_name(ch)==L"SystemTray.ImageIconContent")if(auto g=FN(ch,L"ContainerGrid").try_as<Controls::Grid>())g.Padding(Thickness{});return false;});}
static void STI(FrameworkElement e,int w){auto c=e;if((c=FN(c,L"ContainerGrid"))&&(c=FN(c,L"ContentGrid"))&&(c=FC(c,L"SystemTray.TextIconContent"))&&(c=FN(c,L"ContainerGrid")))if(auto g=c.try_as<Controls::Grid>()){int p=(w>32)?(8+w-32)/2:(w<24)?std::max((8+w-24)/2,0):4;g.Padding(Thickness{(double)p,0,(double)p,0});}}
static void SG(FrameworkElement sp,int r,int w){double ih=0;if(r>1){double g=std::fmax(sp.ActualHeight()-16*r,0.0);ih=16+(static_cast<int>(g/(r+1))/2*2);}int n=Media::VisualTreeHelper::GetChildrenCount(sp),c=(n+r-1)/r,idx=0;EC(sp,[&](auto ch){int i=idx++;if(r>1){ch.Height(ih);Media::TranslateTransform t;t.X(w*((i%c)-i));t.Y(ih*(i/c)-ih*(r-1)/2);ch.RenderTransform(t);}else{auto d=ch.as<DependencyObject>();d.ClearValue(FrameworkElement::HeightProperty());d.ClearValue(UIElement::RenderTransformProperty());}return false;});if(r>1)sp.Width(w*((idx+r-1)/r));else sp.as<DependencyObject>().ClearValue(FrameworkElement::WidthProperty());}
static void SNA(FrameworkElement a,int r,int w){FrameworkElement sp=nullptr,c=a;if((c=FC(c,L"Windows.UI.Xaml.Controls.ItemsPresenter"))&&(c=FC(c,L"Windows.UI.Xaml.Controls.StackPanel")))sp=c;if(!sp)return;EC(sp,[w](auto c){if(winrt::get_class_name(c)!=L"Windows.UI.Xaml.Controls.ContentPresenter")return false;if(auto i=FN(c,L"NotifyItemIcon"))SI(i,w);return false;});SG(sp,r,w);}
static bool SCC(FrameworkElement b,int w){FrameworkElement sp=nullptr,c=b;if((c=FC(c,L"Windows.UI.Xaml.Controls.Grid"))&&(c=FN(c,L"ContentPresenter"))&&(c=FC(c,L"Windows.UI.Xaml.Controls.ItemsPresenter"))&&(c=FC(c,L"Windows.UI.Xaml.Controls.StackPanel")))sp=c;if(!sp)return false;EC(sp,[w](auto c){if(winrt::get_class_name(c)!=L"Windows.UI.Xaml.Controls.ContentPresenter")return false;if(auto i=FN(c,L"SystemTrayIcon"))STI(i,w);return false;});return true;}
static bool SS(PCWSTR n,FrameworkElement ct,int w){FrameworkElement sp=nullptr,c=ct;if((c=FN(c,L"Content"))&&(c=FN(c,L"IconStack"))&&(c=FC(c,L"Windows.UI.Xaml.Controls.ItemsPresenter"))&&(c=FC(c,L"Windows.UI.Xaml.Controls.StackPanel")))sp=c;if(!sp)return false;EC(sp,[n,w](auto c){if(winrt::get_class_name(c)!=L"Windows.UI.Xaml.Controls.ContentPresenter")return false;if(wcscmp(n,L"NotifyIconStack")==0){if(auto ch=FC(c,L"SystemTray.ChevronIconView"))SI(ch,w);}else{if(auto i=FN(c,L"SystemTrayIcon"))SI(i,w);}return false;});return true;}
static void SOV(FrameworkElement r){Controls::WrapGrid wg=nullptr;auto c=r;if((c=FC(c,L"Windows.UI.Xaml.Controls.ItemsControl"))&&(c=FC(c,L"Windows.UI.Xaml.Controls.ItemsPresenter"))&&(c=FC(c,L"Windows.UI.Xaml.Controls.WrapGrid")))wg=c.try_as<Controls::WrapGrid>();if(!wg)return;wg.ItemWidth(OV_W);wg.ItemHeight(OV_W);wg.MaximumRowsOrColumns(OV_R);EC(wg,[](auto c){if(winrt::get_class_name(c)!=L"Windows.UI.Xaml.Controls.ContentPresenter")return false;if(auto nv=FC(c,L"SystemTray.NotifyIconView"))SOI(nv,OV_W);return false;});}

// ╔══════════════════════════════════════════════════════════╗
// ║  XamlRoot from bridge windows                            ║
// ╚══════════════════════════════════════════════════════════╝
static void ApplyFromRoot(XamlRoot rt, int w, int r) {
    if (!rt) return;
    auto co = rt.Content().try_as<FrameworkElement>(); if (!co) return;
    auto f = FC(co, L"SystemTray.SystemTrayFrame"); if (!f) return;
    auto g = FN(f, L"SystemTrayFrameGrid"); if (!g) return;
    LOG(L"Applying styles from XamlRoot!");
    if (auto a = FN(g, L"NotificationAreaIcons")) SNA(a,r,w);
    if (auto b = FN(g, L"ControlCenterButton")) SCC(b,w);
    for (auto n : {L"NotifyIconStack",L"MainStack",L"NonActivatableStack"})
        if (auto c = FN(g,n)) SS(n,c,w);
    if (auto ov = FC(g, L"Windows.UI.Xaml.Controls.Grid")) SOV(ov);
    LOG(L"Styles applied!");
}

static XamlRoot FindXamlRootFromBridge() {
    HWND hTray = nullptr;
    EnumWindows([](HWND h, LPARAM l)->BOOL{DWORD p;WCHAR c[32];return(GetWindowThreadProcessId(h,&p)&&p==GetCurrentProcessId()&&GetClassNameW(h,c,32)&&_wcsicmp(c,L"Shell_TrayWnd")==0)?(*(HWND*)l=h,FALSE):TRUE;},(LPARAM)&hTray);
    if (!hTray) return nullptr;

    struct { XamlRoot result; } ctx = {nullptr};
    EnumChildWindows(hTray, [](HWND h, LPARAM l)->BOOL{
        WCHAR c[256]; GetClassNameW(h, c, 256);
        if (!wcsstr(c, L"DesktopWindowContentBridge") && !wcsstr(c, L"Desktop")) return TRUE;
        auto& ctx = *(decltype(ctx)*)l;
        EnumPropsEx(h, [](HWND, LPCWSTR, HANDLE hData, ULONG_PTR lp)->BOOL{
            auto& ctx = *(decltype(ctx)*)lp;
            if (!hData || hData == INVALID_HANDLE_VALUE) return TRUE;
            IUnknown* unk = (IUnknown*)hData;
            __try {
                unk->AddRef(); ULONG r = unk->Release();
                if (r == 0 || r > 100000) return TRUE;
                FrameworkElement fe = nullptr;
                if (SUCCEEDED(unk->QueryInterface(winrt::guid_of<FrameworkElement>(), winrt::put_abi(fe))) && fe && fe.XamlRoot())
                    { ctx.result = fe.XamlRoot(); return FALSE; }
                DependencyObject dep = nullptr;
                if (SUCCEEDED(unk->QueryInterface(winrt::guid_of<DependencyObject>(), winrt::put_abi(dep))) && dep)
                    if (auto fe2 = dep.try_as<FrameworkElement>()) if (fe2 && fe2.XamlRoot())
                        { ctx.result = fe2.XamlRoot(); return FALSE; }
            } __except(EXCEPTION_EXECUTE_HANDLER) {}
            return TRUE;
        }, (LPARAM)&ctx);
        return ctx.result ? FALSE : TRUE;
    }, (LPARAM)&ctx);
    return ctx.result;
}

// ╔══════════════════════════════════════════════════════════╗
// ║  CreateWindowExW hook                                   ║
// ╚══════════════════════════════════════════════════════════╝
using CWET = HWND(WINAPI*)(DWORD,LPCWSTR,LPCWSTR,DWORD,int,int,int,int,HWND,HMENU,HINSTANCE,LPVOID);
static CWET Real_CW = nullptr;

static HWND WINAPI Hook_CW(DWORD es, LPCWSTR cls, LPCWSTR n, DWORD s, int x, int y, int w, int h, HWND p, HMENU m, HINSTANCE i, LPVOID l) {
    HWND r = Real_CW(es, cls, n, s, x, y, w, h, p, m, i, l);
    if (r && cls && wcsstr(cls, L"DesktopWindowContentBridge")) {
        LOG(L"Bridge created: %s", cls);
        SetTimer(nullptr, 0, 300, [](HWND, UINT, UINT_PTR id, DWORD) {
            KillTimer(nullptr, id);
            auto xr = FindXamlRootFromBridge();
            if (xr) { LOG(L"Got XamlRoot from hook!"); ApplyFromRoot(xr, ICON_W, ICON_R); }
        });
    }
    return r;
}

// ╔══════════════════════════════════════════════════════════╗
// ║  Worker / Main                                           ║
// ╚══════════════════════════════════════════════════════════╝
static std::atomic<bool> g_u{false}; static UINT_PTR g_tm=0;
static VOID CALLBACK OT(HWND,UINT,UINT_PTR id,DWORD){if(g_u){KillTimer(nullptr,id);return;}auto xr=FindXamlRootFromBridge();if(xr)ApplyFromRoot(xr,ICON_W,ICON_R);}
DWORD WINAPI Wkr(LPVOID){
    LOG(L"=== DLL loaded ===");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MH_Initialize();
    void* target=GetProcAddress(GetModuleHandleW(L"user32.dll"),"CreateWindowExW");
    if(MH_CreateHook(target,Hook_CW,(void**)&Real_CW)==MH_OK) MH_EnableHook(target);
    auto xr=FindXamlRootFromBridge();
    if(xr){LOG(L"Got XamlRoot!");ApplyFromRoot(xr,ICON_W,ICON_R);}else LOG(L"No XamlRoot from props");
    g_tm=SetTimer(nullptr,0,2000,OT);
    MSG m; while(!g_u&&GetMessageW(&m,nullptr,0,0)){TranslateMessage(&m);DispatchMessage(&m);}
    if(g_tm)KillTimer(nullptr,g_tm); MH_Uninitialize(); CoUninitialize(); return 0;
}
BOOL APIENTRY DllMain(HMODULE h,DWORD r,LPVOID){
    if(r==DLL_PROCESS_ATTACH){DisableThreadLibraryCalls(h);if(auto h2=CreateThread(nullptr,0,Wkr,nullptr,0,nullptr))CloseHandle(h2);}
    else if(r==DLL_PROCESS_DETACH){g_u=true;if(g_tm)KillTimer(nullptr,g_tm);}
    return TRUE;
}
