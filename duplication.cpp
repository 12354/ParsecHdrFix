// IDXGIOutputDuplication implementation backed by borderless FP16 WGC.
#include "api.h"
#include "app/cap_hdr.h"
#include "capture_source.h"
#include "app/log.h"
#include <wrl/client.h>
#include <new>
#include <cstring>
#include <vector>
using Microsoft::WRL::ComPtr;

// WGC cursor capture is off, so the pointer is reported through the documented
// DXGI pointer fields and the consumer draws it as its own cursor plane.
struct CURSOR_SAMPLE { HCURSOR handle=nullptr; bool visible=false; POINT position {}; };

static bool cursor_bits(HDC dc,HBITMAP bitmap,LONG width,LONG height,WORD bpp,std::vector<BYTE> &out,UINT &stride)
{
    struct { BITMAPINFOHEADER header; RGBQUAD colors[2]; } info {};
    info.header.biSize=sizeof(info.header); info.header.biWidth=width; info.header.biHeight=-height;
    info.header.biPlanes=1; info.header.biBitCount=bpp; info.header.biCompression=BI_RGB;
    stride=((UINT(width)*bpp+31)/32)*4; out.assign(size_t(stride)*height,0);
    return GetDIBits(dc,bitmap,0,UINT(height),out.data(),reinterpret_cast<BITMAPINFO *>(&info),DIB_RGB_COLORS)==height;
}

// Converts an HCURSOR to the DXGI_OUTDUPL_POINTER_SHAPE_TYPE layouts.
static bool cursor_shape(HCURSOR handle,std::vector<BYTE> &shape,DXGI_OUTDUPL_POINTER_SHAPE_INFO &info)
{
    ICONINFO icon {}; if(!GetIconInfo(handle,&icon)) return false;
    struct OWNER { ICONINFO &icon; HDC dc=CreateCompatibleDC(nullptr);
        ~OWNER() { if(dc) DeleteDC(dc); if(icon.hbmMask) DeleteObject(icon.hbmMask); if(icon.hbmColor) DeleteObject(icon.hbmColor); } } owner {icon};
    BITMAP mask {};
    if(!owner.dc || !icon.hbmMask || !GetObjectW(icon.hbmMask,sizeof(mask),&mask)) return false;
    info={}; info.HotSpot={LONG(icon.xHotspot),LONG(icon.yHotspot)};
    std::vector<BYTE> mask_bits; UINT mask_stride=0;
    if(!icon.hbmColor) {
        // AND plane above XOR plane, rows packed to whole bytes as DXGI delivers them.
        if(mask.bmWidth<=0 || mask.bmWidth>256 || mask.bmHeight<=0 || mask.bmHeight>512) return false;
        if(!cursor_bits(owner.dc,icon.hbmMask,mask.bmWidth,mask.bmHeight,1,mask_bits,mask_stride)) return false;
        info.Type=DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
        info.Width=UINT(mask.bmWidth); info.Height=UINT(mask.bmHeight); info.Pitch=(info.Width+7)/8;
        shape.resize(size_t(info.Pitch)*info.Height);
        for(UINT y=0;y<info.Height;++y) memcpy(&shape[size_t(y)*info.Pitch],&mask_bits[size_t(y)*mask_stride],info.Pitch);
        return true;
    }
    BITMAP color {};
    if(!GetObjectW(icon.hbmColor,sizeof(color),&color) || color.bmWidth<=0 || color.bmWidth>256 || color.bmHeight<=0 || color.bmHeight>256) return false;
    UINT stride=0;
    if(!cursor_bits(owner.dc,icon.hbmColor,color.bmWidth,color.bmHeight,32,shape,stride)) return false;
    info.Width=UINT(color.bmWidth); info.Height=UINT(color.bmHeight); info.Pitch=stride;
    bool alpha=false;
    for(size_t i=3;i<shape.size();i+=4) if(shape[i]) { alpha=true; break; }
    if(alpha) { info.Type=DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR; return true; }
    // No alpha: AND mask bit set means XOR with the screen (alpha 0xFF), else replace (alpha 0).
    if(!cursor_bits(owner.dc,icon.hbmMask,color.bmWidth,color.bmHeight,1,mask_bits,mask_stride)) return false;
    info.Type=DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR;
    for(UINT y=0;y<info.Height;++y) for(UINT x=0;x<info.Width;++x)
        shape[size_t(y)*stride+x*4+3]=(mask_bits[size_t(y)*mask_stride+x/8]&(0x80>>(x&7)))?0xff:0x00;
    return true;
}

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
    bool acquired=false, lost=false, delivered=false, pointer_only=false;
    RECT bounds {};
    CURSOR_SAMPLE cursor;
    bool cursor_known=false;
    std::vector<BYTE> shape;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info {};

    CURSOR_SAMPLE sample_cursor() {
        CURSORINFO ci {sizeof(ci)};
        if(!GetCursorInfo(&ci)) return cursor; // e.g. secure desktop: keep the last state.
        CURSOR_SAMPLE now;
        now.handle=ci.hCursor;
        now.visible=ci.hCursor && (ci.flags&CURSOR_SHOWING) && !(ci.flags&CURSOR_SUPPRESSED) && PtInRect(&bounds,ci.ptScreenPos);
        now.position={ci.ptScreenPos.x-bounds.left,ci.ptScreenPos.y-bounds.top};
        return now;
    }
    bool cursor_changed(const CURSOR_SAMPLE &now) const {
        return !cursor_known || now.handle!=cursor.handle || now.visible!=cursor.visible;
    }
    // Like DXGI: LastMouseUpdateTime and the shape size are set only when they change.
    void report_cursor(const CURSOR_SAMPLE &now,DXGI_OUTDUPL_FRAME_INFO *info) {
        info->PointerPosition.Position=now.position; info->PointerPosition.Visible=now.visible;
        if(!cursor_changed(now)) return;
        QueryPerformanceCounter(&info->LastMouseUpdateTime);
        if(!cursor_known || now.handle!=cursor.handle) {
            shape.clear(); shape_info={};
            if(now.handle && !cursor_shape(now.handle,shape,shape_info)) { shape.clear(); shape_info={}; }
            info->PointerShapeBufferSize=UINT(shape.size());
        }
        cursor=now; cursor_known=true;
    }
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
        bounds=od.DesktopCoordinates;
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
        const CURSOR_SAMPLE now=sample_cursor();
        // WGC has no frame for pointer-only changes; DXGI reports them with LastPresentTime 0.
        const bool fresh=hr!=DXGI_ERROR_WAIT_TIMEOUT;
        if(!fresh && (!delivered || !cursor_changed(now))) return hr;
        if(fresh) {
            if(SUCCEEDED(hr)) hr=cap_hdr_convert(mapper,frame.GetAddressOf(),desc.Rotation);
            if(FAILED(hr)) { lost=true; core_log(0,"HDR API v3: capture lost (0x%08lx), requesting duplication recreation",hr); return DXGI_ERROR_ACCESS_LOST; }
            D3D11_TEXTURE2D_DESC actual {}; frame->GetDesc(&actual);
            if(actual.Width!=desc.ModeDesc.Width || actual.Height!=desc.ModeDesc.Height) { lost=true; return DXGI_ERROR_ACCESS_LOST; }
        }
        // Producer key 0 -> consumer key 1. Holding the consumer mutex until
        // ReleaseFrame preserves the original duplication ownership contract.
        hr=writer->AcquireSync(0,1000);
        if(hr!=S_OK) { lost=true; return DXGI_ERROR_ACCESS_LOST; }
        if(fresh) context->CopyResource(shared.Get(),frame.Get());
        hr=writer->ReleaseSync(1);
        if(hr!=S_OK) { lost=true; return DXGI_ERROR_ACCESS_LOST; }
        hr=reader->AcquireSync(1,1000);
        if(hr!=S_OK) { lost=true; return DXGI_ERROR_ACCESS_LOST; }
        acquired=true; pointer_only=!fresh;
        if(fresh) {
            info->AccumulatedFrames=1; QueryPerformanceCounter(&info->LastPresentTime);
            info->TotalMetadataBufferSize=sizeof(RECT);
        }
        report_cursor(now,info);
        *out=resource.Get(); (*out)->AddRef();
        if(fresh) InterlockedIncrement(&hdr_frames);
        if(fresh && !delivered) { delivered=true;core_log(1,"HDR API v3: first tone-mapped frame delivered (%ux%u)",desc.ModeDesc.Width,desc.ModeDesc.Height); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFrameDirtyRects(UINT size,RECT *rects,UINT *required) override {
        if(!required) return E_INVALIDARG; *required=pointer_only?0:sizeof(RECT);
        if(!acquired) return DXGI_ERROR_INVALID_CALL;
        if(pointer_only) return S_OK;
        if(size<sizeof(RECT)) return DXGI_ERROR_MORE_DATA;
        if(!rects) return E_INVALIDARG;
        *rects={0,0,LONG(desc.ModeDesc.Width),LONG(desc.ModeDesc.Height)}; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFrameMoveRects(UINT,DXGI_OUTDUPL_MOVE_RECT *,UINT *required) override {
        if(!required) return E_INVALIDARG; *required=0;
        return acquired?S_OK:DXGI_ERROR_INVALID_CALL;
    }
    HRESULT STDMETHODCALLTYPE GetFramePointerShape(UINT size,void *buffer,UINT *required,DXGI_OUTDUPL_POINTER_SHAPE_INFO *info) override {
        if(!required||!info) return E_INVALIDARG; *required=UINT(shape.size()); *info={};
        if(!acquired) return DXGI_ERROR_INVALID_CALL;
        if(shape.empty()) return S_OK;
        if(size<shape.size()) return DXGI_ERROR_MORE_DATA;
        if(!buffer) return E_INVALIDARG;
        memcpy(buffer,shape.data(),shape.size()); *info=shape_info; return S_OK;
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
