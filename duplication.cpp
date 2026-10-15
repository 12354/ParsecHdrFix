// IDXGIOutputDuplication implementation backed by borderless FP16 WGC.
#include "api.h"
#include "app/cap_hdr.h"
#include "capture_source.h"
#include "app/log.h"
#include <wrl/client.h>
#include <new>
#include <cstring>
using Microsoft::WRL::ComPtr;

class HDR_DUPLICATION final : public IDXGIOutputDuplication {
    volatile LONG refs=1;
    ComPtr<IDXGIOutput> output;
    ComPtr<ID3D11Device> consumer, producer;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> shared, visible;
    ComPtr<IDXGIKeyedMutex> writer, reader;
    ComPtr<IDXGIResource> resource;
    CAP_HDR *mapper=nullptr;
    SHIM_CAPTURE *capture=nullptr;
    DXGI_OUTDUPL_DESC desc {};
    bool acquired=false, lost=false, delivered=false;
public:
    ~HDR_DUPLICATION() {
        if(acquired) reader->ReleaseSync(0);
        shim_capture_destroy(&capture); cap_hdr_destroy(&mapper);
    }
    HRESULT initialize(IDXGIOutput *monitor,IUnknown *device) {
        output=monitor;
        HRESULT hr=device->QueryInterface(IID_PPV_ARGS(&consumer)); if(FAILED(hr)) return hr;
        DXGI_OUTPUT_DESC od {}; hr=output->GetDesc(&od); if(FAILED(hr)) return hr;
        ComPtr<IDXGIDevice> dxgi; hr=consumer.As(&dxgi); if(FAILED(hr)) return hr;
        ComPtr<IDXGIAdapter> adapter, output_adapter;
        hr=dxgi->GetAdapter(&adapter); if(FAILED(hr)) return hr;
        hr=output->GetParent(IID_PPV_ARGS(&output_adapter)); if(FAILED(hr)) return hr;
        DXGI_ADAPTER_DESC a {},b {}; adapter->GetDesc(&a); output_adapter->GetDesc(&b);
        if(memcmp(&a.AdapterLuid,&b.AdapterLuid,sizeof(LUID))) return E_INVALIDARG;
        // Own context avoids changing the caller's rendering/encoding state.
        hr=D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,0,D3D11_SDK_VERSION,&producer,nullptr,&context);
        if(FAILED(hr)) return hr;
        mapper=cap_hdr_create(producer.Get(),output.Get());
        if(!mapper) return E_OUTOFMEMORY;
        if(!cap_hdr_enabled(mapper)) return DXGI_ERROR_ACCESS_LOST;
        desc.Rotation=od.Rotation>=DXGI_MODE_ROTATION_IDENTITY && od.Rotation<=DXGI_MODE_ROTATION_ROTATE270 ? od.Rotation : DXGI_MODE_ROTATION_IDENTITY;
        desc.ModeDesc.Width=od.DesktopCoordinates.right-od.DesktopCoordinates.left;
        desc.ModeDesc.Height=od.DesktopCoordinates.bottom-od.DesktopCoordinates.top;
        if(desc.Rotation==DXGI_MODE_ROTATION_ROTATE90 || desc.Rotation==DXGI_MODE_ROTATION_ROTATE270) {
            UINT w=desc.ModeDesc.Width; desc.ModeDesc.Width=desc.ModeDesc.Height; desc.ModeDesc.Height=w;
        }
        desc.ModeDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.ModeDesc.RefreshRate={0,1}; desc.DesktopImageInSystemMemory=FALSE;
        D3D11_TEXTURE2D_DESC td {};
        td.Width=desc.ModeDesc.Width; td.Height=desc.ModeDesc.Height;
        td.MipLevels=td.ArraySize=1; td.Format=desc.ModeDesc.Format; td.SampleDesc.Count=1;
        td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        td.MiscFlags=D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        hr=producer->CreateTexture2D(&td,nullptr,&shared); if(FAILED(hr)) return hr;
        ComPtr<IDXGIResource> exported; hr=shared.As(&exported); if(FAILED(hr)) return hr;
        HANDLE handle=nullptr; hr=exported->GetSharedHandle(&handle); if(FAILED(hr)) return hr;
        // Legacy shared handles from GetSharedHandle are not NT handles to close.
        hr=consumer->OpenSharedResource(handle,IID_PPV_ARGS(&visible)); if(FAILED(hr)) return hr;
        hr=shared.As(&writer); if(FAILED(hr)) return hr;
        hr=visible.As(&reader); if(FAILED(hr)) return hr;
        hr=visible.As(&resource); if(FAILED(hr)) return hr;
        // Strict borderless: failure occurs before StartCapture. Caller uses native DXGI.
        return shim_capture_create(&capture,producer.Get(),od.Monitor);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out) override {
        if(!out) return E_POINTER; *out=nullptr;
        if(iid==__uuidof(IUnknown)||iid==__uuidof(IDXGIObject)||iid==__uuidof(IDXGIOutputDuplication)) {
            *out=static_cast<IDXGIOutputDuplication *>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { LONG n=InterlockedDecrement(&refs); if(!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID name,UINT size,const void *data) override { return visible->SetPrivateData(name,size,data); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID name,const IUnknown *data) override { return visible->SetPrivateDataInterface(name,data); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID name,UINT *size,void *data) override { return visible->GetPrivateData(name,size,data); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID iid,void **out) override { return output->QueryInterface(iid,out); }
    void STDMETHODCALLTYPE GetDesc(DXGI_OUTDUPL_DESC *out) override { if(out) *out=desc; }
    HRESULT STDMETHODCALLTYPE AcquireNextFrame(UINT timeout,DXGI_OUTDUPL_FRAME_INFO *info,IDXGIResource **out) override {
        if(!info || !out) return E_INVALIDARG;
        *out=nullptr; *info={};
        if(acquired) return DXGI_ERROR_INVALID_CALL;
        if(lost || !cap_hdr_refresh(mapper)) return DXGI_ERROR_ACCESS_LOST;
        ComPtr<ID3D11Texture2D> frame;
        HRESULT hr=shim_capture_acquire(capture,timeout,&frame);
        if(hr==DXGI_ERROR_WAIT_TIMEOUT) return hr;
        if(SUCCEEDED(hr)) hr=cap_hdr_convert(mapper,frame.GetAddressOf(),desc.Rotation);
        if(FAILED(hr)) { lost=true; core_log(0,"HDR API v3: capture lost (0x%08lx), requesting duplication recreation",hr); return DXGI_ERROR_ACCESS_LOST; }
        D3D11_TEXTURE2D_DESC actual {}; frame->GetDesc(&actual);
        if(actual.Width!=desc.ModeDesc.Width || actual.Height!=desc.ModeDesc.Height) { lost=true; return DXGI_ERROR_ACCESS_LOST; }
        // Producer key 0 -> consumer key 1. Holding the consumer mutex until
        // ReleaseFrame preserves the original duplication ownership contract.
        hr=writer->AcquireSync(0,1000);
        if(hr!=S_OK) { lost=true; return DXGI_ERROR_ACCESS_LOST; }
        context->CopyResource(shared.Get(),frame.Get());
        hr=writer->ReleaseSync(1);
        if(hr!=S_OK) { lost=true; return DXGI_ERROR_ACCESS_LOST; }
        hr=reader->AcquireSync(1,1000);
        if(hr!=S_OK) { lost=true; return DXGI_ERROR_ACCESS_LOST; }
        acquired=true;
        info->AccumulatedFrames=1; QueryPerformanceCounter(&info->LastPresentTime);
        info->LastMouseUpdateTime=info->LastPresentTime;
        info->PointerPosition.Visible=FALSE; // WGC includes the cursor in video.
        info->TotalMetadataBufferSize=sizeof(RECT);
        *out=resource.Get(); (*out)->AddRef(); InterlockedIncrement(&hdr_frames);
        if(!delivered) { delivered=true;core_log(1,"HDR API v3: first tone-mapped frame delivered (%ux%u)",desc.ModeDesc.Width,desc.ModeDesc.Height); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFrameDirtyRects(UINT size,RECT *rects,UINT *required) override {
        if(!required) return E_INVALIDARG; *required=sizeof(RECT);
        if(!acquired) return DXGI_ERROR_INVALID_CALL;
        if(size<sizeof(RECT)) return DXGI_ERROR_MORE_DATA;
        if(!rects) return E_INVALIDARG;
        *rects={0,0,LONG(desc.ModeDesc.Width),LONG(desc.ModeDesc.Height)}; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFrameMoveRects(UINT,DXGI_OUTDUPL_MOVE_RECT *,UINT *required) override {
        if(!required) return E_INVALIDARG; *required=0;
        return acquired?S_OK:DXGI_ERROR_INVALID_CALL;
    }
    HRESULT STDMETHODCALLTYPE GetFramePointerShape(UINT,void *,UINT *required,DXGI_OUTDUPL_POINTER_SHAPE_INFO *info) override {
        if(!required||!info) return E_INVALIDARG; *required=0; *info={};
        return acquired?S_OK:DXGI_ERROR_INVALID_CALL;
    }
    HRESULT STDMETHODCALLTYPE MapDesktopSurface(DXGI_MAPPED_RECT *out) override {
        if(!out) return E_INVALIDARG; *out={}; return acquired?DXGI_ERROR_UNSUPPORTED:DXGI_ERROR_INVALID_CALL;
    }
    HRESULT STDMETHODCALLTYPE UnMapDesktopSurface() override { return DXGI_ERROR_INVALID_CALL; }
    HRESULT STDMETHODCALLTYPE ReleaseFrame() override {
        if(!acquired) return DXGI_ERROR_INVALID_CALL;
        acquired=false; HRESULT hr=reader->ReleaseSync(0);
        if(hr!=S_OK) { lost=true; return DXGI_ERROR_ACCESS_LOST; }
        return S_OK;
    }
};

HRESULT hdr_duplication_create(IDXGIOutput *output,IUnknown *device,IDXGIOutputDuplication **out)
{
    if(!output||!device||!out) return E_INVALIDARG;
    *out=nullptr;
    ComPtr<IDXGIOutput6> hdr_output; DXGI_OUTPUT_DESC1 desc {};
    if(FAILED(output->QueryInterface(IID_PPV_ARGS(&hdr_output))) || FAILED(hdr_output->GetDesc1(&desc)) ||
        desc.ColorSpace!=DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) return S_FALSE;
    auto *p=new(std::nothrow) HDR_DUPLICATION;
    if(!p) return E_OUTOFMEMORY;
    HRESULT hr;
    try { hr=p->initialize(output,device); } catch(...) { hr=E_FAIL; }
    if(FAILED(hr)) { p->Release(); return hr; }
    *out=p; InterlockedIncrement(&hdr_sessions);
    core_log(1,"HDR API v3 ACTIVE: Windows DXGI hook -> borderless full FP16 scRGB -> SDR BGRA");
    return S_OK;
}
