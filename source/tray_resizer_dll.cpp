// tray_resizer_dll.cpp - with file logging + combined approach

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

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shlwapi.lib")

using namespace winrt::Windows::UI::Xaml;
constexpr int ICON_W=24, ICON_R=1, OV_W=32, OV_R=5;

// File logging
static FILE* g_log = nullptr;
static void LOG(const wchar_t* fmt, ...) {
    va_list args; va_start(args, fmt);
    WCHAR buf[1024]; wvsprintfW(buf, fmt, args);
    va_end(args);
    OutputDebugStringW(buf);
    if (!g_log) _wfopen_s(&g_log, L"\\Temp\\tray_resizer.log", L"a");
    if (g_log) { fwprintf(g_log, L"%s\n", buf); fflush(g_log); }
}

// ╔══════════════════════════════════════════════════════════╗
// ║  APPROACH 1: DbgHelp Symbol Server (same as Windhawk)  ║
// ╚══════════════════════════════════════════════════════════╝
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

using GTH = void*(WINAPI*)(void*,void**);
using DEC = void(WINAPI*)(void*);
static GTH g_th=nullptr; static DEC g_dc=nullptr; static size_t g_off=0x48; static void* g_vt=nullptr;

static void* FindSym(const char* names[]) {
    HANDLE hp=GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME|SYMOPT_DEFERRED_LOADS);
    SymInitialize(hp, "srv*\\Temp\\Symbols*https://msdl.microsoft.com/download/symbols", FALSE);
    
    uint8_t buf[sizeof(SYMBOL_INFO)+512];
    auto si=(SYMBOL_INFO*)buf; si->SizeOfStruct=sizeof(SYMBOL_INFO); si->MaxNameLen=512;
    
    for(int n=0; names[n]; n++) {
        if(SymFromName(hp, names[n], si)) {
            uint8_t* a=(uint8_t*)si->Address;
            if(a[0]==0xE9) a=a+5+*(int32_t*)(a+1);
            LOG(L"Sym found: %S = %p", names[n], a);
            return a;
        }
    }
    return nullptr;
}

// ╔══════════════════════════════════════════════════════════╗
// ║  APPROACH 2: RTTI                                       ║
// ╚══════════════════════════════════════════════════════════╝
#pragma pack(push,1)
struct RTTITD{const void* vf;void* spare;char name[1];};
struct RTTICOL{DWORD sig,off,cd;RTTITD* td;void* cd2;};
#pragma pack(pop)

struct Sec{const uint8_t* s;size_t sz;};
static Sec GetS(HMODULE m,const char* n){
    auto* b=(const uint8_t*)m;auto* nt=(IMAGE_NT_HEADERS*)(b+((IMAGE_DOS_HEADER*)b)->e_lfanew);auto* s=IMAGE_FIRST_SECTION(nt);
    for(WORD i=0;i<nt->FileHeader.NumberOfSections;i++) if(memcmp(s[i].Name,n,8)==0) return {b+s[i].VirtualAddress,s[i].SizeOfRawData};
    return{0,0};
}

static void* FindVTableRTTI(HMODULE mod, const char* className) {
    auto r=GetS(mod,".rdata"); if(!r.s) return nullptr;
    auto* base=(const uint8_t*)mod;
    auto* nt=(IMAGE_NT_HEADERS*)(base+((IMAGE_DOS_HEADER*)base)->e_lfanew);
    size_t nl=strlen(className);
    
    LOG(L"Scanning for RTTI: %S", className);
    
    for(size_t i=0;i<r.sz-nl;i++) {
        if(memcmp(r.s+i,className,nl)!=0) continue;
        auto* td=(RTTITD*)(r.s+i-16);
        if(td->vf==nullptr||td->vf==(void*)-1) continue; // likely not a TypeDescriptor
        LOG(L"Found TypeDescriptor at +0x%X",(int)((uint8_t*)td-base));
        
        // Look for COL pointer that references this TypeDescriptor
        for(size_t j=0;j<r.sz-8;j++) {
            auto* col=*(RTTICOL**)(r.s+j);
            if(col->sig>1||col->off>0x1000||col->cd>0x1000) continue;
            // Check col address is within module
            if((uint8_t*)col<base||(uint8_t*)col>=base+nt->OptionalHeader.SizeOfImage) continue;
            // Check signature
            if(col->sig!=0&&col->sig!=1) continue;
            // Check if this COL references our TypeDescriptor
            if(col->td==td) {
                // vtable[-1] = &COL, so vtable = r.s+j+8
                void* vt=(void*)(r.s+j+8);
                LOG(L"Found vtable at +0x%X",(int)((uint8_t*)vt-base));
                return vt;
            }
        }
    }
    LOG(L"RTTI NOT FOUND: %S", className);
    return nullptr;
}

// ╔══════════════════════════════════════════════════════════╗
// ║  Initialize - try multiple approaches                   ║
// ╚══════════════════════════════════════════════════════════╝

static bool Init() {
    if(g_th) return true;
    
    HMODULE mod=GetModuleHandleW(L"taskbar.dll");
    if(!mod) mod=LoadLibraryExW(L"taskbar.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!mod){LOG(L"taskbar.dll not found");return false;}
    LOG(L"taskbar.dll at %p",mod);

    // Approach A: DbgHelp Symbol Server
    LOG(L"Trying DbgHelp symbol resolution...");
    const char* names[] = {"CTaskBand::GetTaskbarHost", "?GetTaskbarHost@CTaskBand@@UEAA?AV?$shared_ptr@VTaskbarHost@@@std@@XZ", nullptr};
    g_th=(GTH)FindSym(names);
    const char* fhNames[] = {"TaskbarHost::FrameHeight", "?FrameHeight@TaskbarHost@@QEAAHXZ", nullptr};
    auto* fh=FindSym(fhNames);
    const char* decNames[] = {"std::_Ref_count_base::_Decref", "?_Decref@_Ref_count_base@std@@QEAAXXZ", nullptr};
    g_dc=(DEC)FindSym(decNames);
    if(fh){
        uint8_t*c=(uint8_t*)fh;if(c[0]==0xE9)c=c+5+*(int32_t*)(c+1);
        for(int i=0;i<40;i++){if(c[i]==0x48&&c[i+1]==0x83&&c[i+2]==0xC1){g_off=c[i+3];break;}if(c[i]==0x48&&c[i+1]==0x8D&&c[i+2]==0x41){g_off=c[i+3];break;}}
    }

    // Approach B: RTTI (if DbgHelp failed)
    if(!g_th) {
        LOG(L"Trying RTTI approach...");
        auto* vt=FindVTableRTTI(mod, ".?AVCTaskBand@@");
        if(!vt) vt=FindVTableRTTI(mod, "CTaskBand");
        if(!vt) vt=FindVTableRTTI(mod, "CTaskBand");
        
        if(vt) {
            auto** vtbl=(void**)vt;
            for(int i=0;i<30;i++) {
                if(!vtbl[i]) continue;
                uint8_t* func=(uint8_t*)vtbl[i];
                if(func[0]==0xE9) func=func+5+*(int32_t*)(func+1);
                // Check for lea rcx,[rbx+XX] or lea rcx,[rdi+XX]
                for(int j=0;j<80;j++) {
                    uint8_t off=0;
                    if(func[j]==0x48&&func[j+1]==0x8D&& (func[j+2]==0x4B||func[j+2]==0x4F)) off=func[j+3];
                    else if(func[j]==0x48&&func[j+1]==0x8D&&func[j+2]==0x41) off=func[j+3];
                    else continue;
                    if(off>=0x20&&off<=0x80) {
                        g_th=(GTH)vtbl[i]; g_off=off;
                        LOG(L"GetTaskbarHost: vtable[%d] off=0x%X",i,(int)off);
                        i=30; break;
                    }
                }
            }
        }
    }
    
    // Approach C: Pattern scanning fallback
    if(!g_th) {
        LOG(L"Trying pattern scanning...");
        auto txt=GetS(mod,".text");
        if(txt.s) {
            for(int offTry=0x30;offTry<=0x70;offTry++){
                for(size_t i=0;i<txt.sz-40;i++){
                    if(txt.s[i]==0x48&&txt.s[i+1]==0x89&&txt.s[i+2]==0x5C&&txt.s[i+3]==0x24&&txt.s[i+5]==0x57&&txt.s[i+6]==0x48&&txt.s[i+7]==0x83&&txt.s[i+8]==0xEC&&txt.s[i+9]==0x20&&txt.s[i+10]==0x48&&txt.s[i+11]==0x8B&&txt.s[i+12]==0xD9){
                        for(int j=13;j<40;j++){if(txt.s[i+j]==0x48&&txt.s[i+j+1]==0x8D&&(txt.s[i+j+2]==0x4B||txt.s[i+j+2]==0x4F)&&txt.s[i+j+3]==(uint8_t)offTry){g_th=(GTH)(txt.s+i);g_off=offTry;LOG(L"Pattern found! off=0x%X",offTry);i=txt.sz;break;}}
                    }
                }
            }
        }
    }
    
    // Find vtable reference in .rdata
    if(g_th){
        auto r=GetS(mod,".rdata");
        if(r.s){
            auto ptr=(const uint8_t*)&g_th;
            for(size_t i=0;i<r.sz-8;i++){
                if(memcmp(r.s+i,ptr,8)==0){
                    auto* e=const_cast<uint8_t*>(r.s+i);
                    for(int w=2;w<20;w++){auto*c=e-w*8;if(c<r.s)break;if(*(uint8_t**)(c-8)>=(uint8_t*)mod&&*(uint8_t**)(c-8)<(uint8_t*)mod+0x200000){g_vt=c;break;}}
                    break;
                }
            }
        }
    }
    
    // Find decref
    if(!g_dc){
        auto txt=GetS(mod,".text");
        if(txt.s){
            for(size_t i=0;i<txt.sz-5;i++){
                if(txt.s[i]==0xF0&&(txt.s[i+2]==0x41||txt.s[i+2]==0x49)&&txt.s[i+3]==0x04){
                    auto* p=txt.s+i; for(int j=0;j<80&&p>txt.s;j--){p--;if(p[0]==0x48&&p[1]==0x89&&p[2]==0x5C){g_dc=(DEC)p;break;}if(p[0]==0x48&&p[1]==0x83&&p[2]==0xEC&&p[3]<=0x40&&p>=txt.s){g_dc=(DEC)p;break;}}
                    if(!g_dc)g_dc=(DEC)(txt.s+i);
                    break;
                }
            }
        }
    }
    
    LOG(L"Init: g_th=%s g_dc=%s g_vt=%s off=0x%X",g_th?"OK":"NO",g_dc?"OK":"NO",g_vt?"OK":"NO",(int)g_off);
    return g_th!=nullptr;
}

// ╔══════════════════════════════════════════════════════════╗
// ║  Get XAML Root                                          ║
// ╚══════════════════════════════════════════════════════════╝
static HWND FindTW(){
    HWND r=nullptr;EnumWindows([](HWND h,LPARAM l)->BOOL{DWORD p;WCHAR c[32];return(GetWindowThreadProcessId(h,&p)&&p==GetCurrentProcessId()&&GetClassNameW(h,c,32)&&_wcsicmp(c,L"Shell_TrayWnd")==0)?(*(HWND*)l=h,FALSE):TRUE;},(LPARAM)&r);return r;
}

static XamlRoot GetRoot(HWND h){
    if(!Init()){LOG(L"Init failed");return nullptr;}
    HWND s=(HWND)GetPropW(h,L"TaskbandHWND");if(!s){LOG(L"No TaskbandHWND");return nullptr;}
    auto* tb=(void*)GetWindowLongPtrW(s,0);if(!tb){LOG(L"No CTaskBand ptr");return nullptr;}
    
    void* itf=tb;bool f=false;
    if(g_vt){for(int i=0;i<25;i++){if(*(void**)((void**)tb+i)==g_vt){itf=(void**)tb+i;f=true;break;}}}
    if(!f&&g_th){for(int i=0;i<25;i++){auto* vt=*(void***)((void**)tb+i);if(!vt)continue;for(int j=0;j<30;j++){if(vt[j]==(void*)g_th){itf=(void**)tb+i;f=true;break;}}if(f)break;}}
    if(!f){LOG(L"VTable not found");return nullptr;}
    
    void* sp[2]={};g_th(itf,sp);
    if(!sp[0]){LOG(L"GetTaskbarHost failed");return nullptr;}
    auto* unk=*(IUnknown**)((BYTE*)sp[0]+g_off);
    if(!unk){LOG(L"No IUnknown at +0x%X",(int)g_off);if(g_dc)g_dc(sp[1]);return nullptr;}
    FrameworkElement te=nullptr;unk->QueryInterface(winrt::guid_of<FrameworkElement>(),winrt::put_abi(te));
    if(!te){LOG(L"QI failed");if(g_dc)g_dc(sp[1]);return nullptr;}
    XamlRoot r=te.XamlRoot();if(g_dc)g_dc(sp[1]);
    LOG(L"XamlRoot=%p",r);
    return r;
}

// ╔══════════════════════════════════════════════════════════╗
// ║  XAML HELPERS & STYLES                                   ║
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

static void Apply(int w,int r){
    LOG(L"=== Apply ===");
    auto h=FindTW();if(!h){LOG(L"No tray");return;}
    auto rt=GetRoot(h);if(!rt){LOG(L"No XamlRoot");return;}
    auto co=rt.Content().try_as<FrameworkElement>();if(!co){LOG(L"No content");return;}
    auto f=FC(co,L"SystemTray.SystemTrayFrame");if(!f){LOG(L"No SystemTrayFrame");return;}
    auto g=FN(f,L"SystemTrayFrameGrid");if(!g){LOG(L"No SystemTrayFrameGrid");return;}
    LOG(L"Applying styles!");
    if(auto a=FN(g,L"NotificationAreaIcons"))SNA(a,r,w);
    if(auto b=FN(g,L"ControlCenterButton"))SCC(b,w);
    for(auto n:{L"NotifyIconStack",L"MainStack",L"NonActivatableStack"})if(auto c=FN(g,n))SS(n,c,w);
    if(auto ov=FC(g,L"Windows.UI.Xaml.Controls.Grid"))SOV(ov);
    LOG(L"Done");
}

static std::atomic<bool> g_u{false};static UINT_PTR g_tm=0;
static VOID CALLBACK OT(HWND,UINT,UINT_PTR id,DWORD){if(g_u){KillTimer(nullptr,id);return;}Apply(ICON_W,ICON_R);}
DWORD WINAPI Wkr(LPVOID){
    _wfreopen_s(&g_log,L"\\Temp\\tray_resizer.log",L"w",stdout);
    LOG(L"=== TRAY RESIZER DLL LOADED ===");
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    Apply(ICON_W,ICON_R);
    g_tm=SetTimer(nullptr,0,2000,OT);
    MSG m;while(!g_u&&GetMessageW(&m,nullptr,0,0)){TranslateMessage(&m);DispatchMessage(&m);}
    if(g_tm)KillTimer(nullptr,g_tm);CoUninitialize();
    if(g_log)fclose(g_log);
    return 0;
}
BOOL APIENTRY DllMain(HMODULE h,DWORD r,LPVOID){
    if(r==DLL_PROCESS_ATTACH){DisableThreadLibraryCalls(h);if(auto h2=CreateThread(nullptr,0,Wkr,nullptr,0,nullptr))CloseHandle(h2);}
    else if(r==DLL_PROCESS_DETACH){g_u=true;if(g_tm)KillTimer(nullptr,g_tm);if(g_log)fclose(g_log);}
    return TRUE;
}
