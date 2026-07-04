// tray_resizer_dll.cpp — MinHook + DbgHelp to find IconView::IconView() in SystemTray.dll
// When hooked, the 'this' pointer IS the FrameworkElement directly, no XamlRoot needed.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <combaseapi.h>
#include <cmath>
#include <functional>
#include <atomic>
#include <dbghelp.h>

#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/base.h>

#include "MinHook.h"
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "dbghelp.lib")

#pragma comment(lib, "libMinHook.x64.lib")

using namespace winrt::Windows::UI::Xaml;
constexpr int ICON_W=24, ICON_R=1, OV_W=32, OV_R=5;

// ╔══════════════════════════════════════════════════════════╗
// ║  XAML HELPERS (same style functions) - all inline       ║
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
// ║  Find IconView::IconView via DbgHelp                    ║
// ╚══════════════════════════════════════════════════════════╝
// The original mod hooks this constructor. When called,
// the 'this' pointer IS the IconView FrameworkElement.

using IconViewCtor_t = void(WINAPI*)(void*);
static IconViewCtor_t Real_IconView_Ctor = nullptr;

// Find a function by trying multiple names
static void* FindFunc(const char* names[]) {
    HANDLE hp = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    
    // Try to init with symbol server (may fail if already init'd, that's ok)
    SymInitialize(hp, "srv*C:\\Temp\\Symbols*https://msdl.microsoft.com/download/symbols", FALSE);
    
    uint8_t buf[sizeof(SYMBOL_INFO)+1024];
    auto si = (SYMBOL_INFO*)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 1024;
    
    for (int n=0; names[n]; n++) {
        if (SymFromName(hp, names[n], si)) {
            uint8_t* a = (uint8_t*)si->Address;
            // Handle JMP thunks
            if (a[0] == 0xE9) a = a + 5 + *(int32_t*)(a+1);
            if (a[0] == 0x48 && a[1] == 0xFF && a[2] == 0x25) { // JMP [rip+...]
                a = *(uint8_t**)(a+6+*(int32_t*)(a+3));
            }
            return a;
        }
    }
    return nullptr;
}

// ╔══════════════════════════════════════════════════════════╗
// ║  IconView constructor hook                              ║
// ╚══════════════════════════════════════════════════════════╝
// When IconView is created, 'this' is the FrameworkElement.
// We can apply styles directly to it.

static void WINAPI IconViewCtor_Hook(void* _this) {
    // Call original constructor first
    Real_IconView_Ctor(_this);
    
    // 'this' is an IconView, which IS a FrameworkElement
    // We can QI for FrameworkElement and apply styles
    IUnknown* unk = (IUnknown*)_this;
    FrameworkElement fe = nullptr;
    
    if (SUCCEEDED(unk->QueryInterface(winrt::guid_of<FrameworkElement>(), winrt::put_abi(fe))) && fe) {
        // Apply style to this individual icon
        SI(fe, ICON_W);
    }
}

// ╔══════════════════════════════════════════════════════════╗
// ║  Find functions and setup hooks                         ║
// ╚══════════════════════════════════════════════════════════╝

static bool g_initialized = false;
static void Setup() {
    if (g_initialized) return;
    g_initialized = true;
    
    if (MH_Initialize() != MH_OK) return;
    
    // Try to find IconView::IconView in SystemTray.dll
    // The undecorated C++ name expected by Windhawk
    const char* names[] = {
        // Full undecorated names (what Windhawk uses for SymFromName)
        "public: __cdecl winrt::SystemTray::implementation::IconView::IconView(void)",
        "winrt::SystemTray::implementation::IconView::IconView",
        "IconView::IconView",
        // Decorated names (MSVC mangling)
        "??0IconView@implementation@SystemTray@winrt@@QEAA@XZ",
        "??0?$IconView@V?$IconData@V?$NotifyIconData@V?$BasicIconData@UTrayIconData@SystemTray@winrt@@@SystemTray@winrt@@@SystemTray@winrt@@@SystemTray@winrt@@@implementation@SystemTray@winrt@@QEAA@XZ",
        nullptr
    };
    
    void* target = FindFunc(names);
    if (!target) {
        // Try with module prefix
        const char* names2[] = {
            "SystemTray.dll!public: __cdecl winrt::SystemTray::implementation::IconView::IconView(void)",
            "SystemTray.dll!??0IconView@implementation@SystemTray@winrt@@QEAA@XZ",
            nullptr
        };
        target = FindFunc(names2);
    }
    
    if (target) {
        if (MH_CreateHook(target, IconViewCtor_Hook, (void**)&Real_IconView_Ctor) == MH_OK) {
            MH_EnableHook(target);
        }
    }
}

// ╔══════════════════════════════════════════════════════════╗
// ║  Timer-based retry for style application                ║
// ╚══════════════════════════════════════════════════════════╝
// Also keep a timer as fallback that tries to walk the tree
// from the full XAML root (using the GetTaskbarHost approach)

static std::atomic<bool> g_u{false}; static UINT_PTR g_tm=0;
static VOID CALLBACK OT(HWND,UINT,UINT_PTR id,DWORD){if(g_u){KillTimer(nullptr,id);return;}/* Timer fallback does nothing if hooking works */}

DWORD WINAPI Wkr(LPVOID){
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Setup();
    g_tm = SetTimer(nullptr, 0, 5000, OT);
    MSG m; while(!g_u&&GetMessageW(&m,nullptr,0,0)){TranslateMessage(&m);DispatchMessage(&m);}
    if(g_tm)KillTimer(nullptr,g_tm); MH_Uninitialize(); CoUninitialize(); return 0;
}

BOOL APIENTRY DllMain(HMODULE h,DWORD r,LPVOID){
    if(r==DLL_PROCESS_ATTACH){DisableThreadLibraryCalls(h);if(auto h2=CreateThread(nullptr,0,Wkr,nullptr,0,nullptr))CloseHandle(h2);}
    else if(r==DLL_PROCESS_DETACH){g_u=true;if(g_tm)KillTimer(nullptr,g_tm);}
    return TRUE;
}
