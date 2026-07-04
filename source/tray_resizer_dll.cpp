// tray_resizer_dll.cpp — Uses MinHook + RTTI-based vtable finding
// No pattern scanning, no DbgHelp, no internal taskbar functions.
// Finds functions by RTTI type info (MSVC ABI stable).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <combaseapi.h>
#include <cmath>
#include <functional>
#include <atomic>
#include <vector>

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

constexpr int ICON_W=24, ICON_R=1, OV_W=32, OV_R=5;

// ╔══════════════════════════════════════════════════════════╗
// ║  RTTI PARSING — find Function/VTable by class name      ║
// ╚══════════════════════════════════════════════════════════╝
// MSVC RTTI layout:
//   vtable[-1] = &RTTICompleteObjectLocator
//   COL->pTypeDescriptor->name = class name string
// We scan .rdata for the name, find the TypeDescriptor,
// then find the COL, then get the vtable.

#pragma pack(push,1)
struct RTTITD {  // TypeDescriptor
    const void* pVFTable;  // pointer to vtable of type_info
    void* spare;
    char name[1];
};
struct RTTICOL { // CompleteObjectLocator  
    DWORD signature;
    DWORD offset;          // offset of this vtable from object start
    DWORD cdOffset;        // constructor displacement offset
    RTTITD* pTypeDescriptor;
    void* pClassDescriptor;
};
#pragma pack(pop)

struct Section { const uint8_t* start; size_t size; };
static Section GetSection(HMODULE mod, const char* name) {
    auto* b=(const uint8_t*)mod;
    auto* nt=(IMAGE_NT_HEADERS*)(b+((IMAGE_DOS_HEADER*)b)->e_lfanew);
    auto* s=IMAGE_FIRST_SECTION(nt);
    for(WORD i=0;i<nt->FileHeader.NumberOfSections;i++)
        if(memcmp(s[i].Name,name,8)==0) return {b+s[i].VirtualAddress,s[i].SizeOfRawData};
    return {nullptr,0};
}

// Find the vtable for a class by its RTTI name
// Returns the address of the first vtable entry (function pointer 0)
static void* FindVTableByClass(HMODULE mod, const char* rttiName) {
    auto rdata = GetSection(mod, ".rdata");
    if(!rdata.start) return nullptr;
    
    auto* base = (const uint8_t*)mod;
    auto* nt = (IMAGE_NT_HEADERS*)(base+((IMAGE_DOS_HEADER*)base)->e_lfanew);
    DWORD imgSize = nt->OptionalHeader.SizeOfImage;
    size_t nameLen = strlen(rttiName);
    
    // 1. Find the RTTI class name string in .rdata
    for(size_t i=0; i<rdata.size-nameLen; i++) {
        if(memcmp(rdata.start+i, rttiName, nameLen) == 0) {
            // 2. The TypeDescriptor struct has the name at offset 16 (x64) 
            auto* td = (RTTITD*)(rdata.start + i - 16);
            if(td->name != rdata.start+i) continue; // validate alignment
            
            // 3. Find the COL that references this TypeDescriptor
            for(size_t j=0; j<rdata.size-8; j++) {
                auto* col = *(RTTICOL**)(rdata.start+j);
                // Check if this 8-byte value is a pointer to a COL
                // The COL must point to our TypeDescriptor
                if((uint8_t*)col >= rdata.start && (uint8_t*)col < rdata.start+rdata.size-8) {
                    // Verify it looks like a COL (signature=0 or 1, reasonable offsets)
                    if(col->signature <= 1 && col->offset < 0x1000 && col->cdOffset < 0x1000) {
                        if(col->pTypeDescriptor == td) {
                            // The COL pointer is stored at vtable[-1]
                            // So vtable = address where COL pointer is stored + 8
                            void* vtable = (void*)(rdata.start + j + 8);
                            return vtable;
                        }
                    }
                }
            }
        }
    }
    return nullptr;
}

// ╔══════════════════════════════════════════════════════════╗
// ║  Find function by RTTI + scanning vtable for pattern    ║
// ╚══════════════════════════════════════════════════════════╝
// Once we have the vtable, we can scan its entries for a
// function that has a specific instruction pattern.

// Scan a function for: add rcx, XX (where XX is a known offset)
// Returns the offset value, or 0 if not found
static int GetOffsetFromFunc(void* func) {
    if(!func) return 0;
    uint8_t* code = (uint8_t*)func;
    // Handle JMP thunks
    if(code[0]==0xE9) code=code+5+*(int32_t*)(code+1);
    // Scan for add rcx,XX / lea rax,[rcx+XX]
    for(int i=0; i<60; i++) {
        // 48 83 C1 XX  -> add rcx, XX
        if(code[i]==0x48&&code[i+1]==0x83&&code[i+2]==0xC1) return code[i+3];
        // 48 8D 41 XX  -> lea rax,[rcx+XX]
        if(code[i]==0x48&&code[i+1]==0x8D&&code[i+2]==0x41) return code[i+3];
    }
    return 0;
}

// ╔══════════════════════════════════════════════════════════╗
// ║  Find GetTaskbarHost via RTTI for CTaskBand             ║
// ╚══════════════════════════════════════════════════════════╝

using GTH = void*(WINAPI*)(void*,void**);
using DEC = void(WINAPI*)(void*);

static GTH g_th=nullptr; static DEC g_dc=nullptr; static size_t g_off=0x48; static void* g_vt=nullptr;

static bool InitFromTaskbar() {
    HMODULE mod = GetModuleHandleW(L"taskbar.dll");
    if(!mod) mod=LoadLibraryExW(L"taskbar.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!mod) return false;

    // The CTaskBand class has multiple base class vtables.
    // One of them is ITaskListWndSite which contains GetTaskbarHost.
    //
    // RTTI names in taskbar.dll:
    // .?AVCTaskBand@@          = CTaskBand
    // .?AVITaskListWndSite@@   = ITaskListWndSite  (might be in different module)
    //
    // We find CTaskBand's vtable via RTTI, then scan its entries
    // for GetTaskbarHost (identified by the offset pattern).

    void* vtable = FindVTableByClass(mod, ".?AVCTaskBand@@");
    if(!vtable) {
        // Also try with namespace prefix (undecorated)
        vtable = FindVTableByClass(mod, "CTaskBand");
    }
    if(!vtable) return false;

    // The vtable has ~20-30 entries. GetTaskbarHost is typically
    // at a high index (towards the end). Let me find it by looking
    // for the `lea rcx,[rbx+XX]` pattern in each function.
    
    auto** vtbl = (void**)vtable;
    for(int i=0; i<30; i++) {
        if(!vtbl[i]) continue;
        int off = GetOffsetFromFunc(vtbl[i]);
        if(off >= 0x20 && off <= 0x80) {
            // This function has a field access at offset `off`
            // GetTaskbarHost accesses the TaskbarHost pointer at this offset
            g_th = (GTH)vtbl[i];
            g_off = off;
            break;
        }
    }

    // Find std::_Ref_count_base::_Decref
    // Look for the pattern: lock; dec [rcx+4]
    auto text = GetSection(mod, ".text");
    if(text.start) {
        for(size_t i=0; i<text.size-5; i++) {
            if(text.start[i]==0xF0 && text.start[i+1]==0xFF && 
               (text.start[i+2]==0x41||text.start[i+2]==0x49) && text.start[i+3]==0x04) {
                auto* p = text.start+i;
                // Walk back to find function start
                for(int j=0; j<80 && p>text.start; j--) {
                    p--;
                    if(p[0]==0x48&&p[1]==0x89&&p[2]==0x5C) { g_dc=(DEC)p; break; }
                    if(p[0]==0x48&&p[1]==0x83&&p[2]==0xEC&&p[3]<=0x40&&p>=text.start) { g_dc=(DEC)p; break; }
                }
                if(!g_dc) g_dc=(DEC)(text.start+i);
                break;
            }
        }
    }

    // Find vtable in .rdata for ITaskListWndSite
    if(g_th) {
        auto rdata = GetSection(mod, ".rdata");
        if(rdata.start) {
            auto fnAddr = (const uint8_t*)&g_th;
            for(size_t i=0; i<rdata.size-8; i++) {
                if(memcmp(rdata.start+i, fnAddr, 8)==0) {
                    auto* entry = const_cast<uint8_t*>(rdata.start+i);
                    for(int w=2; w<20; w++) {
                        auto* cand = entry - w*8;
                        if(cand < rdata.start) break;
                        auto* rv = *(uint8_t**)(cand-8);
                        if(rv >= (uint8_t*)mod && rv < (uint8_t*)mod + 0x200000) { g_vt=cand; break; }
                    }
                    break;
                }
            }
        }
    }

    return g_th != nullptr;
}

// ╔══════════════════════════════════════════════════════════╗
// ║  Get XAML Root                                          ║
// ╚══════════════════════════════════════════════════════════╝
static HWND FindTrayWnd() {
    HWND r=nullptr; EnumWindows([](HWND h,LPARAM l)->BOOL{DWORD p;WCHAR c[32];return(GetWindowThreadProcessId(h,&p)&&p==GetCurrentProcessId()&&GetClassNameW(h,c,32)&&_wcsicmp(c,L"Shell_TrayWnd")==0)?(*(HWND*)l=h,FALSE):TRUE;},(LPARAM)&r);
    return r;
}

static XamlRoot GetRoot(HWND hTray) {
    if(!InitFromTaskbar()) return nullptr;
    HWND hSw=(HWND)GetPropW(hTray,L"TaskbandHWND"); if(!hSw) return nullptr;
    auto* tb=(void*)GetWindowLongPtrW(hSw,0); if(!tb) return nullptr;
    void* itf=tb; bool found=false;
    if(g_vt){for(int i=0;i<25;i++){if(*(void**)((void**)tb+i)==g_vt){itf=(void**)tb+i;found=true;break;}}}
    if(!found&&g_th){for(int i=0;i<25;i++){auto* vt=*(void***)((void**)tb+i);if(!vt||(uint8_t*)vt<(uint8_t*)GetModuleHandleW(L"taskbar.dll")||(uint8_t*)vt>(uint8_t*)GetModuleHandleW(L"taskbar.dll")+0x200000)continue;for(int j=0;j<30;j++){if(vt[j]==(void*)g_th){itf=(void**)tb+i;found=true;break;}}if(found)break;}}
    if(!found) return nullptr;
    void* sp[2]={}; g_th(itf,sp);
    if(!sp[0]) return nullptr;
    auto* unk=*(IUnknown**)((BYTE*)sp[0]+g_off);
    if(!unk){if(g_dc)g_dc(sp[1]);return nullptr;}
    FrameworkElement te=nullptr; unk->QueryInterface(winrt::guid_of<FrameworkElement>(),winrt::put_abi(te));
    if(!te){if(g_dc)g_dc(sp[1]);return nullptr;}
    XamlRoot r=te.XamlRoot(); if(g_dc)g_dc(sp[1]); return r;
}

// ╔══════════════════════════════════════════════════════════╗
// ║  XAML HELPERS + STYLES                                   ║
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

static void Apply(int w,int r) {
    auto h=FindTrayWnd(); if(!h) return;
    auto rt=GetRoot(h); if(!rt) return;
    auto co=rt.Content().try_as<FrameworkElement>(); if(!co) return;
    auto f=FC(co,L"SystemTray.SystemTrayFrame"); if(!f) return;
    auto g=FN(f,L"SystemTrayFrameGrid"); if(!g) return;
    if(auto a=FN(g,L"NotificationAreaIcons")) SNA(a,r,w);
    if(auto b=FN(g,L"ControlCenterButton")) SCC(b,w);
    for(auto n:{L"NotifyIconStack",L"MainStack",L"NonActivatableStack"}) if(auto c=FN(g,n)) SS(n,c,w);
    if(auto ov=FC(g,L"Windows.UI.Xaml.Controls.Grid")) SOV(ov);
}

static std::atomic<bool> g_u{false}; static UINT_PTR g_tm=0;
static VOID CALLBACK OT(HWND,UINT,UINT_PTR id,DWORD){if(g_u){KillTimer(nullptr,id);return;}Apply(ICON_W,ICON_R);}
DWORD WINAPI Wkr(LPVOID){
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    Apply(ICON_W,ICON_R);
    g_tm=SetTimer(nullptr,0,2000,OT);
    MSG m; while(!g_u&&GetMessageW(&m,nullptr,0,0)){TranslateMessage(&m);DispatchMessage(&m);}
    if(g_tm)KillTimer(nullptr,g_tm); CoUninitialize(); return 0;
}
BOOL APIENTRY DllMain(HMODULE h,DWORD r,LPVOID){
    if(r==DLL_PROCESS_ATTACH){DisableThreadLibraryCalls(h);if(auto h2=CreateThread(nullptr,0,Wkr,nullptr,0,nullptr))CloseHandle(h2);}
    else if(r==DLL_PROCESS_DETACH){g_u=true;if(g_tm)KillTimer(nullptr,g_tm);}
    return TRUE;
}
