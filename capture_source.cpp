// WGC is a per-user service. SYSTEM hosts use this DLL's user-session worker.
#include "capture_source.h"
#include "app/cap_wgc.h"
#include "app/log.h"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <sddl.h>
#include <string>
#include <new>
using Microsoft::WRL::ComPtr;

namespace {
constexpr ULONG protocol=0x48445233;
struct INIT { ULONG magic=protocol; LUID adapter {}; UINT64 monitor=0; };
struct READY { HRESULT hr=E_FAIL; ULONG magic=protocol; UINT64 texture=0; };
struct REQUEST { ULONG magic=protocol; UINT timeout=0; };
struct HANDLE_OWNER {
    HANDLE value=nullptr;
    ~HANDLE_OWNER() { if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value); }
};
bool is_system() {
    HANDLE_OWNER token; BYTE buffer[256];DWORD bytes=0;
    return OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value)&&
        GetTokenInformation(token.value,TokenUser,buffer,sizeof(buffer),&bytes)&&
        IsWellKnownSid(reinterpret_cast<TOKEN_USER *>(buffer)->User.Sid,WinLocalSystemSid);
}
HRESULT last_error() { DWORD error=GetLastError();return HRESULT_FROM_WIN32(error?error:ERROR_GEN_FAILURE); }
// Overlapped server transfers are bounded and wake when the helper exits.
HRESULT transfer(HANDLE pipe,HANDLE process,bool write,void *data,DWORD size,DWORD timeout) {
    HANDLE_OWNER event;event.value=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!event.value)return last_error();
    OVERLAPPED ov {};ov.hEvent=event.value;DWORD bytes=0;
    BOOL ok=write?WriteFile(pipe,data,size,&bytes,&ov):ReadFile(pipe,data,size,&bytes,&ov);
    if(!ok&&GetLastError()!=ERROR_IO_PENDING)return last_error();
    if(!ok) {
        HANDLE waits[]={event.value,process};DWORD wait=WaitForMultipleObjects(2,waits,FALSE,timeout);
        if(wait!=WAIT_OBJECT_0) { CancelIoEx(pipe,&ov);GetOverlappedResult(pipe,&ov,&bytes,TRUE);return wait==WAIT_TIMEOUT?HRESULT_FROM_WIN32(ERROR_TIMEOUT):E_ABORT; }
        if(!GetOverlappedResult(pipe,&ov,&bytes,FALSE))return last_error();
    }
    return bytes==size?S_OK:E_FAIL;
}
bool worker_transfer(HANDLE pipe,bool write,void *data,DWORD size) {
    DWORD bytes=0;return (write?WriteFile(pipe,data,size,&bytes,nullptr):ReadFile(pipe,data,size,&bytes,nullptr))&&bytes==size;
}
}

struct SHIM_CAPTURE {
    CAP_WGC *local=nullptr;
    HANDLE_OWNER pipe,process;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<IDXGIKeyedMutex> mutex;
    bool held=false;
    ~SHIM_CAPTURE() {
        if(held)mutex->ReleaseSync(0);
        cap_wgc_destroy(&local);
        // Closing the pipe makes the worker exit and close its WGC session.
        if(pipe.value&&pipe.value!=INVALID_HANDLE_VALUE) { CloseHandle(pipe.value);pipe.value=nullptr; }
        if(process.value)WaitForSingleObject(process.value,5000);
    }
};

static HRESULT create_worker(SHIM_CAPTURE *p,ID3D11Device *device,HMONITOR monitor) {
    DWORD session=0;if(!ProcessIdToSessionId(GetCurrentProcessId(),&session)||!session)return E_ACCESSDENIED;
    HANDLE_OWNER user;if(!WTSQueryUserToken(session,&user.value))return last_error();
    BYTE buffer[256];DWORD bytes=0;if(!GetTokenInformation(user.value,TokenUser,buffer,sizeof(buffer),&bytes))return last_error();
    LPWSTR sid=nullptr;if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer)->User.Sid,&sid))return last_error();
    std::wstring sddl=L"D:P(A;;GA;;;SY)(A;;GA;;;";sddl+=sid;sddl+=L")";LocalFree(sid);
    PSECURITY_DESCRIPTOR security=nullptr;
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&security,nullptr))return last_error();
    SECURITY_ATTRIBUTES sa {sizeof(sa),security,FALSE};
    GUID id {};HRESULT hr=CoCreateGuid(&id);if(FAILED(hr)){LocalFree(security);return hr;}
    wchar_t guid[40];StringFromGUID2(id,guid,_countof(guid));
    std::wstring name=L"\\\\.\\pipe\\parsec-hdr-";name+=guid;
    p->pipe.value=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,1024,1024,0,&sa);
    LocalFree(security);if(p->pipe.value==INVALID_HANDLE_VALUE)return last_error();
    HMODULE self=nullptr;wchar_t dll[32768],system[MAX_PATH];
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(create_worker),&self)||!GetModuleFileNameW(self,dll,_countof(dll))||!GetSystemDirectoryW(system,_countof(system)))return last_error();
    std::wstring exe=std::wstring(system)+L"\\rundll32.exe";
    std::wstring command=L"\""+exe+L"\" \""+dll+L"\",HdrCaptureWorker "+name;
    void *environment=nullptr;if(!CreateEnvironmentBlock(&environment,user.value,FALSE))return last_error();
    STARTUPINFOW startup {sizeof(startup)};wchar_t desktop[]=L"winsta0\\default";startup.lpDesktop=desktop;
    PROCESS_INFORMATION child {};
    BOOL created=CreateProcessAsUserW(user.value,exe.c_str(),command.data(),nullptr,nullptr,FALSE,
        CREATE_UNICODE_ENVIRONMENT|CREATE_NO_WINDOW,environment,system,&startup,&child);
    DWORD error=GetLastError();DestroyEnvironmentBlock(environment);
    if(!created)return HRESULT_FROM_WIN32(error);
    CloseHandle(child.hThread);p->process.value=child.hProcess;
    HANDLE_OWNER event;event.value=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!event.value)return last_error();
    OVERLAPPED ov {};ov.hEvent=event.value;
    if(!ConnectNamedPipe(p->pipe.value,&ov)) {
        error=GetLastError();
        if(error==ERROR_IO_PENDING) {
            HANDLE waits[]={event.value,p->process.value};DWORD wait=WaitForMultipleObjects(2,waits,FALSE,5000);DWORD transferred=0;
            if(wait!=WAIT_OBJECT_0){CancelIoEx(p->pipe.value,&ov);GetOverlappedResult(p->pipe.value,&ov,&transferred,TRUE);return E_ABORT;}
            if(!GetOverlappedResult(p->pipe.value,&ov,&transferred,FALSE))return last_error();
        } else if(error!=ERROR_PIPE_CONNECTED)return HRESULT_FROM_WIN32(error);
    }
    ULONG client=0;if(!GetNamedPipeClientProcessId(p->pipe.value,&client)||client!=child.dwProcessId)return E_ACCESSDENIED;
    ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC desc {};
    hr=device->QueryInterface(IID_PPV_ARGS(&dxgi));if(FAILED(hr))return hr;
    hr=dxgi->GetAdapter(&adapter);if(FAILED(hr))return hr;hr=adapter->GetDesc(&desc);if(FAILED(hr))return hr;
    INIT init;init.adapter=desc.AdapterLuid;init.monitor=reinterpret_cast<UINT64>(monitor);
    hr=transfer(p->pipe.value,p->process.value,true,&init,sizeof(init),5000);if(FAILED(hr))return hr;
    // This all runs inside Parsec's DuplicateOutput call, so it is kept short:
    // the worker's own permission wait (5 s) plus WGC setup must fit in here.
    READY ready;hr=transfer(p->pipe.value,p->process.value,false,&ready,sizeof(ready),8000);if(FAILED(hr))return hr;
    if(ready.magic!=protocol)return E_FAIL;if(FAILED(ready.hr))return ready.hr;
    hr=device->OpenSharedResource(reinterpret_cast<HANDLE>(ready.texture),IID_PPV_ARGS(&p->texture));if(FAILED(hr))return hr;
    D3D11_TEXTURE2D_DESC td {};p->texture->GetDesc(&td);if(td.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT)return E_FAIL;
    hr=p->texture.As(&p->mutex);if(FAILED(hr))return hr;
    core_log(1,"HDR API v3: user-session FP16 worker ready (PID %lu, session %lu)",child.dwProcessId,session);
    return S_OK;
}

HRESULT shim_capture_create(SHIM_CAPTURE **out,ID3D11Device *device,HMONITOR monitor) {
    if(!out)return E_POINTER;*out=nullptr;
    auto *p=new(std::nothrow) SHIM_CAPTURE;if(!p)return E_OUTOFMEMORY;
    HRESULT hr;
    try { hr=is_system()?create_worker(p,device,monitor):cap_wgc_create(&p->local,device,monitor,true); }
    catch(...) { hr=E_FAIL; }
    if(FAILED(hr)){delete p;return hr;}*out=p;return S_OK;
}
HRESULT shim_capture_acquire(SHIM_CAPTURE *p,UINT timeout,ID3D11Texture2D **out) {
    if(!p||!out)return E_INVALIDARG;*out=nullptr;
    if(p->local)return cap_wgc_acquire(p->local,timeout,out);
    if(p->held) { HRESULT hr=p->mutex->ReleaseSync(0);p->held=false;if(hr!=S_OK)return E_FAIL; }
    for(;;) {
        REQUEST request;request.timeout=timeout==INFINITE?1000:timeout;
        HRESULT hr=transfer(p->pipe.value,p->process.value,true,&request,sizeof(request),5000);if(FAILED(hr))return hr;
        HRESULT frame=E_FAIL;
        DWORD wait=request.timeout>MAXDWORD-5000?MAXDWORD:request.timeout+5000;
        hr=transfer(p->pipe.value,p->process.value,false,&frame,sizeof(frame),wait);if(FAILED(hr))return hr;
        if(frame==DXGI_ERROR_WAIT_TIMEOUT&&timeout==INFINITE)continue;
        if(FAILED(frame))return frame;
        hr=p->mutex->AcquireSync(1,1000);if(hr!=S_OK)return E_FAIL;p->held=true;
        *out=p->texture.Get();(*out)->AddRef();return S_OK;
    }
}
void shim_capture_destroy(SHIM_CAPTURE **out) { if(out){delete *out;*out=nullptr;} }

// rundll32 entry point; runs with the interactive user's primary token, not SYSTEM.
extern "C" __declspec(dllexport) void CALLBACK HdrCaptureWorker(HWND,HINSTANCE,LPSTR args,int) {
    try {
        constexpr char prefix[]="\\\\.\\pipe\\parsec-hdr-";
        if(!args||strncmp(args,prefix,sizeof(prefix)-1)||strlen(args)>100)return;
        HANDLE_OWNER pipe;pipe.value=CreateFileA(args,GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
        if(pipe.value==INVALID_HANDLE_VALUE)return;
        DWORD mode=PIPE_READMODE_MESSAGE;if(!SetNamedPipeHandleState(pipe.value,&mode,nullptr,nullptr))return;
        INIT init;if(!worker_transfer(pipe.value,false,&init,sizeof(init))||init.magic!=protocol)return;
        READY ready;
        ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> adapter;ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
        ComPtr<ID3D11Texture2D> shared;ComPtr<IDXGIKeyedMutex> mutex;CAP_WGC *capture=nullptr;
        struct CLEANUP { CAP_WGC **capture;~CLEANUP(){cap_wgc_destroy(capture);} } cleanup {&capture};
        auto initialize=[&]()->HRESULT {
            HRESULT hr=CreateDXGIFactory1(IID_PPV_ARGS(&factory));if(FAILED(hr))return hr;
            hr=factory->EnumAdapterByLuid(init.adapter,IID_PPV_ARGS(&adapter));if(FAILED(hr))return hr;
            hr=D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context);if(FAILED(hr))return hr;
            HMONITOR monitor=reinterpret_cast<HMONITOR>(init.monitor);MONITORINFO info {sizeof(info)};if(!GetMonitorInfoW(monitor,&info))return last_error();
            D3D11_TEXTURE2D_DESC td {};td.Width=info.rcMonitor.right-info.rcMonitor.left;td.Height=info.rcMonitor.bottom-info.rcMonitor.top;
            td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;td.MiscFlags=D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
            hr=device->CreateTexture2D(&td,nullptr,&shared);if(FAILED(hr))return hr;
            hr=shared.As(&mutex);if(FAILED(hr))return hr;
            ComPtr<IDXGIResource> resource;hr=shared.As(&resource);if(FAILED(hr))return hr;HANDLE handle=nullptr;
            hr=resource->GetSharedHandle(&handle);if(FAILED(hr))return hr;ready.texture=reinterpret_cast<UINT64>(handle);
            return cap_wgc_create(&capture,device.Get(),monitor,true);
        };
        ready.hr=initialize();
        if(!worker_transfer(pipe.value,true,&ready,sizeof(ready))||FAILED(ready.hr))return;
        REQUEST request;
        while(worker_transfer(pipe.value,false,&request,sizeof(request))&&request.magic==protocol) {
            ComPtr<ID3D11Texture2D> frame;HRESULT hr=cap_wgc_acquire(capture,request.timeout,&frame);
            if(SUCCEEDED(hr)) {
                D3D11_TEXTURE2D_DESC actual {},expected {};frame->GetDesc(&actual);shared->GetDesc(&expected);
                if(actual.Width!=expected.Width||actual.Height!=expected.Height||actual.Format!=expected.Format)hr=DXGI_ERROR_ACCESS_LOST;
                else {
                    hr=mutex->AcquireSync(0,1000);
                    if(hr==S_OK){context->CopyResource(shared.Get(),frame.Get());hr=mutex->ReleaseSync(1);}
                    else hr=E_FAIL;
                }
            }
            if(!worker_transfer(pipe.value,true,&hr,sizeof(hr)))break;
            if(FAILED(hr)&&hr!=DXGI_ERROR_WAIT_TIMEOUT)break;
        }
    } catch(...) {}
}
