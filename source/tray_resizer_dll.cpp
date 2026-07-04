// tray_resizer_dll.cpp — fixes: explicitly load symbols with SymLoadModule64

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <combaseapi.h>
#include <unknwn.h>
#include <dbghelp.h>
#include <cmath>
#include <functional>
#include <atomic>

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
#pragma comment(lib, "dbghelp.lib")

using namespace winrt::Windows::UI::Xaml;

constexpr int ICON_W=24, ICON_R=1, OV_W=32, OV_R=5, TMR=1500;

using GTH = void*(WINAPI*)(void*,void**);
using DEC = void(WINAPI*)(void*);
static HMODULE g_t=NULL; static GTH g_th=nullptr; static DEC g_dc=nullptr; static void* g_vt=nullptr; static size_t g_off=0x48;

#define LOG(...) do{WCHAR _b[512];wsprintfW(_b,__VA_ARGS__);OutputDebugStringW(_b);}while(0)

static void* FindSym(const wchar_t* name, const char* decorated) {
    HANDLE hp=GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME|SYMOPT_DEFERRED_LOADS|SYMOPT_LOAD_LINES);

    // Set symbol path to include Microsoft Symbol Server — same as Windhawk!
    // This enables downloading PDBs for system DLLs like taskbar.dll
    SymInitialize(hp, "srv*C:\\Temp\\Symbols*https://msdl.microsoft.com/download/symbols", TRUE);

    char ansi[512];
    WideCharToMultiByte(CP_ACP,0,name,-1,ansi,512,0,0);

    uint8_t buf[sizeof(SYMBOL_INFO)+512];
    auto si=(SYMBOL_INFO*)buf; si->SizeOfStruct=sizeof(SYMBOL_INFO); si->MaxNameLen=512;

    // Try undecorated name (most reliable)
    if (SymFromName(hp, ansi, si)) {
        uint8_t* a=(uint8_t*)si->Address; if(a[0]==0xE9) a=a+5+*(int32_t*)(a+1);
        LOG(L"FOUND '%s' = %p",name,a);
        return a;
    }

    // Try decorated
    if(decorated){
        if(SymFromName(hp,decorated,si)){
            uint8_t* a=(uint8_t*)si->Address; if(a[0]==0xE9) a=a+5+*(int32_t*)(a+1);
            LOG(L"FOUND decorated"); return a;
        }
    }

    // Try module-qualified
    char prefixed[600];
    strcpy_s(prefixed,"taskbar.dll!"); strcat_s(prefixed,ansi);
    if(SymFromName(hp,prefixed,si)){
        uint8_t* a=(uint8_t*)si->Address; if(a[0]==0xE9) a=a+5+*(int32_t*)(a+1);
        LOG(L"FOUND qualified"); return a;
    }

    LOG(L"NOT FOUND: '%s'",name);
    return nullptr;
}

static bool Init() {
    if(g_th) return true;
    g_t=GetModuleHandleW(L"taskbar.dll");
    if(!g_t) g_t=LoadLibraryExW(L"taskbar.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!g_t){LOG(L"taskbar.dll not found");return false;}
    LOG(L"taskbar.dll=%p",g_t);

    g_th=(GTH)FindSym(L"CTaskBand::GetTaskbarHost","?GetTaskbarHost@CTaskBand@@UEAA?AV?$shared_ptr@VTaskbarHost@@@std@@XZ");
    auto* fh=FindSym(L"TaskbarHost::FrameHeight","?FrameHeight@TaskbarHost@@QEAAHXZ");
    g_dc=(DEC)FindSym(L"std::_Ref_count_base::_Decref","?_Decref@_Ref_count_base@std@@QEAAXXZ");

    if(fh){
        uint8_t* c=(uint8_t*)fh; if(c[0]==0xE9) c=c+5+*(int32_t*)(c+1);
        for(int i=0;i<40;i++){
            if(c[i]==0x48&&c[i+1]==0x83&&c[i+2]==0xC1){g_off=c[i+3];break;}
            if(c[i]==0x48&&c[i+1]==0x8D&&c[i+2]==0x41){g_off=c[i+3];break;}
        }
        LOG(L"Offset from FrameHeight: 0x%X",(int)g_off);
    }

    // Find vtable
    if(g_th){
        auto* b=(const uint8_t*)g_t; auto* nt=(IMAGE_NT_HEADERS*)(b+((IMAGE_DOS_HEADER*)b)->e_lfanew);
        auto* sec=IMAGE_FIRST_SECTION(nt);
        const uint8_t* rd=nullptr; size_t rs=0;
        for(WORD i=0;i<nt->FileHeader.NumberOfSections;i++)
            if(memcmp(sec[i].Name,".rdata",6)==0){rd=b+sec[i].VirtualAddress;rs=sec[i].SizeOfRawData;break;}
        if(rd){auto ptr=(const uint8_t*)&g_th; for(size_t i=0;i<rs-8;i++){if(memcmp(rd+i,ptr,8)==0){auto* e=const_cast<uint8_t*>(rd+i); for(int w=1;w<25;w++){auto* c=e-w*8;if(c<rd)break;auto* rv=*(uint8_t**)(c-8);if(rv>=b&&rv<b+nt->OptionalHeader.SizeOfImage){g_vt=c;break;}}if(!g_vt)g_vt=e-8*8;if(g_vt<rd)g_vt=e;break;}}}
    }

    LOG(L"Init: th=%s dc=%s vt=%s off=0x%X",g_th?"OK":"NO",g_dc?"OK":"NO",g_vt?"OK":"NO",(int)g_off);
    return g_th!=nullptr;
}

static HWND FindTW(){
    HWND r=nullptr;EnumWindows([](HWND h,LPARAM l)->BOOL{DWORD p;WCHAR c[32];return(GetWindowThreadProcessId(h,&p)&&p==GetCurrentProcessId()&&GetClassNameW(h,c,32)&&_wcsicmp(c,L"Shell_TrayWnd")==0)?(*(HWND*)l=h,FALSE):TRUE;},(LPARAM)&r);return r;
}

static XamlRoot GetR(HWND h){
    if(!Init())return nullptr;
    HWND s=(HWND)GetPropW(h,L"TaskbandHWND"); if(!s)return nullptr;
    auto* tb=(void*)GetWindowLongPtrW(s,0); if(!tb)return nullptr;
    void* itf=tb; bool f=false;
    if(g_vt){for(int i=0;i<25;i++){if(*(void**)((void**)tb+i)==g_vt){itf=(void**)tb+i;f=true;break;}}}
    if(!f&&g_th){for(int i=0;i<25;i++){auto* vt=*(void***)((void**)tb+i);if(!vt||(uint8_t*)vt<(uint8_t*)g_t||(uint8_t*)vt>(uint8_t*)g_t+0x200000)continue;for(int j=0;j<30;j++){if(vt[j]==(void*)g_th){itf=(void**)tb+i;f=true;break;}}if(f)break;}}
    if(!f){LOG(L"VTable not found");return nullptr;}
    void* sp[2]={}; g_th(itf,sp);
    if(!sp[0]){LOG(L"GetTaskbarHost failed");return nullptr;}
    auto* unk=*(IUnknown**)((BYTE*)sp[0]+g_off);
    if(!unk){LOG(L"No IUnknown at +0x%X",(int)g_off);if(g_dc)g_dc(sp[1]);return nullptr;}
    FrameworkElement te=nullptr; unk->QueryInterface(winrt::guid_of<FrameworkElement>(),winrt::put_abi(te));
    if(!te){LOG(L"QI failed");if(g_dc)g_dc(sp[1]);return nullptr;}
    XamlRoot r=te.XamlRoot(); if(g_dc)g_dc(sp[1]);
    LOG(L"XamlRoot=%p",r);
    return r;
}

// ── XAML Helpers (same style functions) ──
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

static void Apply(int w,int r){
    LOG(L"Apply()");
    auto h=FindTW(); if(!h){LOG(L"No tray window");return;}
    auto rt=GetR(h); if(!rt){LOG(L"No XamlRoot");return;}
    auto co=rt.Content().try_as<FrameworkElement>(); if(!co){LOG(L"No content");return;}
    auto f=FC(co,L"SystemTray.SystemTrayFrame"); if(!f){LOG(L"No SystemTrayFrame");return;}
    auto g=FN(f,L"SystemTrayFrameGrid"); if(!g){LOG(L"No SystemTrayFrameGrid");return;}
    LOG(L"Applying styles...");
    if(auto a=FN(g,L"NotificationAreaIcons")) SNA(a,r,w);
    if(auto b=FN(g,L"ControlCenterButton")) SCC(b,w);
    for(auto n:{L"NotifyIconStack",L"MainStack",L"NonActivatableStack"}) if(auto c=FN(g,n)) SS(n,c,w);
    if(auto ov=FC(g,L"Windows.UI.Xaml.Controls.Grid")) SOV(ov);
    LOG(L"Done");
}

static std::atomic<bool> g_u{false}; static UINT_PTR g_tm=0; static HWINEVENTHOOK g_hk=nullptr;
static void CALLBACK WE(HWINEVENTHOOK,DWORD ev,HWND hwnd,LONG io,LONG,DWORD,DWORD){if(g_u||!hwnd)return;if((ev==EVENT_OBJECT_CREATE||ev==EVENT_OBJECT_SHOW)&&io==OBJID_WINDOW){WCHAR c[32]={};GetClassNameW(hwnd,c,32);if(wcsstr(c,L"NotifyIconOverflow")||wcsstr(c,L"DesktopWindowContentBridge"))Apply(ICON_W,ICON_R);}}
static VOID CALLBACK OT(HWND,UINT,UINT_PTR id,DWORD){if(g_u){KillTimer(nullptr,id);return;}Apply(ICON_W,ICON_R);}
DWORD WINAPI Wkr(LPVOID){
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    Apply(ICON_W,ICON_R);
    g_hk=SetWinEventHook(EVENT_OBJECT_CREATE,EVENT_OBJECT_SHOW,nullptr,WE,GetCurrentProcessId(),0,WINEVENT_OUTOFCONTEXT);
    g_tm=SetTimer(nullptr,0,TMR,OT);
    MSG m; while(!g_u&&GetMessageW(&m,nullptr,0,0)){TranslateMessage(&m);DispatchMessage(&m);}
    if(g_hk)UnhookWinEvent(g_hk); if(g_tm)KillTimer(nullptr,g_tm); CoUninitialize(); return 0;
}
BOOL APIENTRY DllMain(HMODULE h,DWORD r,LPVOID){
    if(r==DLL_PROCESS_ATTACH){DisableThreadLibraryCalls(h);if(auto h2=CreateThread(nullptr,0,Wkr,nullptr,0,nullptr))CloseHandle(h2);}
    else if(r==DLL_PROCESS_DETACH){g_u=true;if(g_hk)UnhookWinEvent(g_hk);if(g_tm)KillTimer(nullptr,g_tm);if(g_t)FreeLibrary(g_t);}
    return TRUE;
}
