// Hooks only documented Windows exports and SDK COM methods. No app addresses.
#include "api.h"
#include "app/log.h"
#include <wrl/client.h>
#include <cstring>
using Microsoft::WRL::ComPtr;
volatile LONG hdr_factories, hdr_duplication_calls, hdr_sessions, hdr_fallbacks, hdr_frames;
static HMODULE own_module;
static SRWLOCK method_lock = SRWLOCK_INIT;
struct METHOD { void **slot; void *original; };
static METHOD methods[512];
static unsigned method_count;
static thread_local unsigned native_duplication;

static void hook_method(IUnknown *object, unsigned index, void *hook)
{
    void **slot = *reinterpret_cast<void ***>(object) + index;
    AcquireSRWLockExclusive(&method_lock);
    if (*slot != hook && method_count < _countof(methods)) {
        bool exists = false;
        for (unsigned i=0; i<method_count; ++i) if (methods[i].slot == slot) exists = true;
        DWORD old;
        if (!exists && VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) {
            methods[method_count++] = {slot, *slot};
            InterlockedExchangePointer(slot, hook);
            DWORD ignored; VirtualProtect(slot, sizeof(*slot), old, &ignored);
        }
    }
    ReleaseSRWLockExclusive(&method_lock);
}
template<class T> static T original_method(IUnknown *object, unsigned index)
{
    void **slot = *reinterpret_cast<void ***>(object) + index;
    void *result = nullptr;
    AcquireSRWLockShared(&method_lock);
    for (unsigned i=0; i<method_count; ++i) if (methods[i].slot == slot) { result = methods[i].original; break; }
    ReleaseSRWLockShared(&method_lock);
    return reinterpret_cast<T>(result);
}
static void hook_adapter(IUnknown *adapter);
static void hook_output(IUnknown *output);

static HRESULT STDMETHODCALLTYPE enum_adapters(IDXGIFactory *self, UINT index, IDXGIAdapter **out)
{
    auto fn = original_method<decltype(&enum_adapters)>(self, slot_factory_enum);
    HRESULT hr = fn ? fn(self,index,out) : E_FAIL;
    if (SUCCEEDED(hr) && out && *out) hook_adapter(*out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE enum_adapters1(IDXGIFactory1 *self, UINT index, IDXGIAdapter1 **out)
{
    auto fn = original_method<decltype(&enum_adapters1)>(self, slot_factory_enum1);
    HRESULT hr = fn ? fn(self,index,out) : E_FAIL;
    if (SUCCEEDED(hr) && out && *out) hook_adapter(*out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE enum_luid(IDXGIFactory4 *self, LUID luid, REFIID iid, void **out)
{
    auto fn = original_method<decltype(&enum_luid)>(self, slot_factory_luid);
    HRESULT hr = fn ? fn(self,luid,iid,out) : E_FAIL;
    if (SUCCEEDED(hr) && out && *out) hook_adapter(static_cast<IUnknown *>(*out));
    return hr;
}
static HRESULT STDMETHODCALLTYPE enum_warp(IDXGIFactory4 *self, REFIID iid, void **out)
{
    auto fn = original_method<decltype(&enum_warp)>(self, slot_factory_warp);
    HRESULT hr = fn ? fn(self,iid,out) : E_FAIL;
    if (SUCCEEDED(hr) && out && *out) hook_adapter(static_cast<IUnknown *>(*out));
    return hr;
}
static HRESULT STDMETHODCALLTYPE enum_gpu(IDXGIFactory6 *self, UINT index, DXGI_GPU_PREFERENCE preference, REFIID iid, void **out)
{
    auto fn = original_method<decltype(&enum_gpu)>(self, slot_factory_gpu);
    HRESULT hr = fn ? fn(self,index,preference,iid,out) : E_FAIL;
    if (SUCCEEDED(hr) && out && *out) hook_adapter(static_cast<IUnknown *>(*out));
    return hr;
}
static HRESULT STDMETHODCALLTYPE enum_outputs(IDXGIAdapter *self, UINT index, IDXGIOutput **out)
{
    auto fn = original_method<decltype(&enum_outputs)>(self, slot_adapter_outputs);
    HRESULT hr = fn ? fn(self,index,out) : E_FAIL;
    if (SUCCEEDED(hr) && out && *out) hook_output(*out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE duplicate(IDXGIOutput1 *self, IUnknown *device, IDXGIOutputDuplication **out)
{
    auto fn = original_method<decltype(&duplicate)>(self, slot_output_duplicate);
    if (!fn) return E_FAIL;
    if (!native_duplication && device && out) {
        InterlockedIncrement(&hdr_duplication_calls);
        HRESULT hr = hdr_duplication_create(self,device,out);
        if (hr == S_OK) return hr;
        if (FAILED(hr)) { InterlockedIncrement(&hdr_fallbacks); core_log(0,"HDR API v3: original DuplicateOutput fallback (0x%08lx)",hr); }
    }
    ++native_duplication; HRESULT hr = fn(self,device,out); --native_duplication;
    return hr;
}
static HRESULT STDMETHODCALLTYPE duplicate1(IDXGIOutput5 *self, IUnknown *device, UINT flags, UINT count,
    const DXGI_FORMAT *formats, IDXGIOutputDuplication **out)
{
    auto fn = original_method<decltype(&duplicate1)>(self, slot_output_duplicate1);
    if (!fn) return E_FAIL;
    bool bgra = false;
    if (formats) for (UINT i=0; i<count; ++i) if (formats[i] == DXGI_FORMAT_B8G8R8A8_UNORM) bgra = true;
    if (!native_duplication && device && out && !flags && bgra) {
        InterlockedIncrement(&hdr_duplication_calls);
        HRESULT hr = hdr_duplication_create(self,device,out);
        if (hr == S_OK) return hr;
        if (FAILED(hr)) { InterlockedIncrement(&hdr_fallbacks); core_log(0,"HDR API v3: original DuplicateOutput1 fallback (0x%08lx)",hr); }
    }
    ++native_duplication; HRESULT hr = fn(self,device,flags,count,formats,out); --native_duplication;
    return hr;
}
static void hook_output(IUnknown *output)
{
    const IID *ids[] = {&__uuidof(IDXGIOutput1),&__uuidof(IDXGIOutput2),&__uuidof(IDXGIOutput3),
        &__uuidof(IDXGIOutput4),&__uuidof(IDXGIOutput5),&__uuidof(IDXGIOutput6)};
    for (unsigned i=0; i<_countof(ids); ++i) {
        ComPtr<IUnknown> part;
        if (SUCCEEDED(output->QueryInterface(*ids[i], reinterpret_cast<void **>(part.GetAddressOf())))) {
            hook_method(part.Get(),slot_output_duplicate,reinterpret_cast<void *>(duplicate));
            if (i>=4) hook_method(part.Get(),slot_output_duplicate1,reinterpret_cast<void *>(duplicate1));
        }
    }
}
static void hook_adapter(IUnknown *adapter)
{
    const IID *ids[] = {&__uuidof(IDXGIAdapter),&__uuidof(IDXGIAdapter1),&__uuidof(IDXGIAdapter2),
        &__uuidof(IDXGIAdapter3),&__uuidof(IDXGIAdapter4)};
    for (const IID *iid : ids) {
        ComPtr<IUnknown> part;
        if (SUCCEEDED(adapter->QueryInterface(*iid,reinterpret_cast<void **>(part.GetAddressOf()))))
            hook_method(part.Get(),slot_adapter_outputs,reinterpret_cast<void *>(enum_outputs));
    }
}
void api_hook_factory(IUnknown *factory)
{
    shim_pin();
    if(InterlockedIncrement(&hdr_factories)==1)
        core_log(1,"HDR API v3: Windows DXGI factory intercepted (PID %lu)",GetCurrentProcessId());
    const IID *ids[] = {&__uuidof(IDXGIFactory),&__uuidof(IDXGIFactory1),&__uuidof(IDXGIFactory2),
        &__uuidof(IDXGIFactory3),&__uuidof(IDXGIFactory4),&__uuidof(IDXGIFactory5),&__uuidof(IDXGIFactory6),&__uuidof(IDXGIFactory7)};
    for (unsigned i=0; i<_countof(ids); ++i) {
        ComPtr<IUnknown> part;
        if (FAILED(factory->QueryInterface(*ids[i],reinterpret_cast<void **>(part.GetAddressOf())))) continue;
        hook_method(part.Get(),slot_factory_enum,reinterpret_cast<void *>(enum_adapters));
        if (i>=1) hook_method(part.Get(),slot_factory_enum1,reinterpret_cast<void *>(enum_adapters1));
        if (i>=4) {
            hook_method(part.Get(),slot_factory_luid,reinterpret_cast<void *>(enum_luid));
            hook_method(part.Get(),slot_factory_warp,reinterpret_cast<void *>(enum_warp));
        }
        if (i>=6) hook_method(part.Get(),slot_factory_gpu,reinterpret_cast<void *>(enum_gpu));
    }
}

static HRESULT WINAPI factory0(REFIID iid, void **out)
{ HRESULT hr=CreateDXGIFactory(iid,out); if(SUCCEEDED(hr)&&out&&*out) api_hook_factory(static_cast<IUnknown *>(*out)); return hr; }
static HRESULT WINAPI factory1(REFIID iid, void **out)
{ HRESULT hr=CreateDXGIFactory1(iid,out); if(SUCCEEDED(hr)&&out&&*out) api_hook_factory(static_cast<IUnknown *>(*out)); return hr; }
static HRESULT WINAPI factory2(UINT flags, REFIID iid, void **out)
{ HRESULT hr=CreateDXGIFactory2(flags,iid,out); if(SUCCEEDED(hr)&&out&&*out) api_hook_factory(static_cast<IUnknown *>(*out)); return hr; }

static void patch_module(HMODULE module, unsigned depth=0);
static FARPROC WINAPI get_proc(HMODULE module, LPCSTR name);
static HMODULE WINAPI load_w(LPCWSTR name) { HMODULE m=LoadLibraryW(name); DWORD e=GetLastError(); patch_module(m); SetLastError(e); return m; }
static HMODULE WINAPI load_a(LPCSTR name) { HMODULE m=LoadLibraryA(name); DWORD e=GetLastError(); patch_module(m); SetLastError(e); return m; }
static HMODULE WINAPI load_ex_w(LPCWSTR name,HANDLE file,DWORD flags) { HMODULE m=LoadLibraryExW(name,file,flags); DWORD e=GetLastError(); if(!(flags&(LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE|LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE))) patch_module(m); SetLastError(e); return m; }
static HMODULE WINAPI load_ex_a(LPCSTR name,HANDLE file,DWORD flags) { HMODULE m=LoadLibraryExA(name,file,flags); DWORD e=GetLastError(); if(!(flags&(LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE|LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE))) patch_module(m); SetLastError(e); return m; }
static void *replacement(const char *name, void *current)
{
    // Match both API name and the resolved Windows function, not any private symbol.
    struct API { const char *name; const wchar_t *module; void *hook; };
    const API apis[] = {
        {"CreateDXGIFactory",L"dxgi.dll",reinterpret_cast<void *>(factory0)},
        {"CreateDXGIFactory1",L"dxgi.dll",reinterpret_cast<void *>(factory1)},
        {"CreateDXGIFactory2",L"dxgi.dll",reinterpret_cast<void *>(factory2)},
        {"GetProcAddress",L"kernel32.dll",reinterpret_cast<void *>(get_proc)},
        {"LoadLibraryW",L"kernel32.dll",reinterpret_cast<void *>(load_w)},
        {"LoadLibraryA",L"kernel32.dll",reinterpret_cast<void *>(load_a)},
        {"LoadLibraryExW",L"kernel32.dll",reinterpret_cast<void *>(load_ex_w)},
        {"LoadLibraryExA",L"kernel32.dll",reinterpret_cast<void *>(load_ex_a)},
    };
    for (const auto &api : apis) if (!strcmp(name,api.name)) {
        HMODULE module=GetModuleHandleW(api.module);
        if(module && current==reinterpret_cast<void *>(GetProcAddress(module,name))) return api.hook;
    }
    return nullptr;
}
static FARPROC WINAPI get_proc(HMODULE module, LPCSTR name)
{
    FARPROC result=GetProcAddress(module,name); DWORD error=GetLastError();
    if (result && reinterpret_cast<uintptr_t>(name)>0xffff) {
        patch_module(module);
        if (void *hook=replacement(name,reinterpret_cast<void *>(result))) result=reinterpret_cast<FARPROC>(hook);
    }
    SetLastError(error); return result;
}
static void patch_module(HMODULE module, unsigned depth)
{
    if (!module || module==own_module || (reinterpret_cast<uintptr_t>(module)&3) || depth>8) return;
    // Leave Windows modules' imports alone. COM method hooks are installed only on
    // factories/outputs actually handed to a consumer through a hooked API.
    wchar_t path[MAX_PATH], windows[MAX_PATH];
    DWORD length=GetModuleFileNameW(module,path,_countof(path)); UINT n=GetWindowsDirectoryW(windows,_countof(windows));
    if (!length || length>=_countof(path) || !n || n>=_countof(windows)) return;
    if (!_wcsnicmp(path,windows,n) && path[n]==L'\\') return;
    auto *base=reinterpret_cast<BYTE *>(module);
    auto *dos=reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<0 || dos->e_lfanew>0x100000) return;
    auto *nt=reinterpret_cast<IMAGE_NT_HEADERS64 *>(base+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC) return;
    const size_t size=nt->OptionalHeader.SizeOfImage;
    auto directory=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if(!directory.VirtualAddress || directory.VirtualAddress>=size || directory.Size>size-directory.VirtualAddress) return;
    auto *imports=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base+directory.VirtualAddress);
    for(unsigned i=0;i<directory.Size/sizeof(*imports)&&imports[i].Name;++i) {
        auto &entry=imports[i];
        if(entry.Name>=size || !entry.OriginalFirstThunk || entry.OriginalFirstThunk>=size || entry.FirstThunk>=size) continue;
        auto *names=reinterpret_cast<IMAGE_THUNK_DATA64 *>(base+entry.OriginalFirstThunk);
        auto *slots=reinterpret_cast<IMAGE_THUNK_DATA64 *>(base+entry.FirstThunk);
        size_t last=entry.OriginalFirstThunk>entry.FirstThunk?entry.OriginalFirstThunk:entry.FirstThunk;
        size_t limit=(size-last)/sizeof(*slots);
        for(size_t j=0;j<limit&&names[j].u1.AddressOfData;++j) {
            if(IMAGE_SNAP_BY_ORDINAL64(names[j].u1.Ordinal) || names[j].u1.AddressOfData>=size-sizeof(IMAGE_IMPORT_BY_NAME)) continue;
            auto *symbol=reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base+names[j].u1.AddressOfData);
            auto **slot=reinterpret_cast<void **>(&slots[j].u1.Function);
            if(void *hook=replacement(symbol->Name,*slot)) {
                DWORD old;
                if(VirtualProtect(slot,sizeof(*slot),PAGE_READWRITE,&old)) {
                    InterlockedExchangePointer(slot,hook); DWORD ignored; VirtualProtect(slot,sizeof(*slot),old,&ignored);
                }
            }
        }
        // Supports non-system dependency DLLs that were already loaded at startup.
        HMODULE dependency=GetModuleHandleA(reinterpret_cast<char *>(base+entry.Name));
        if(dependency && dependency!=module) patch_module(dependency,depth+1);
    }
}
void api_bootstrap(HMODULE self)
{ own_module=self; patch_module(GetModuleHandleW(nullptr)); }
