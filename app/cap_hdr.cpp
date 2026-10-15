// Intentional local functionality, not reconstructed behavior.
#include "cap_hdr.h"
#include "log.h"
#include "cap_hdr_vs.h"
#include "cap_hdr_ps.h"
#include <wrl/client.h>
#include <vector>
#include <cwchar>
#include <new>
#include <cmath>

using Microsoft::WRL::ComPtr;
struct CAP_HDR {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutput6> output;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11Texture2D> input, target;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
    bool enabled = false, warned = false;
    float white = 1;
    ULONGLONG checked = 0;
    DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_UNSPECIFIED;
};

static float cap_hdr_white(const wchar_t *name)
{
    UINT32 np = 0, nm = 0;
    // Topology can change between sizing and querying.
    for (int retry = 0; retry < 3; ++retry) {
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) break;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
        LONG result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr);
        if (result == ERROR_INSUFFICIENT_BUFFER) continue;
        if (result != ERROR_SUCCESS) break;
        for (UINT32 i = 0; i < np; ++i) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
            source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source), paths[i].sourceInfo.adapterId, paths[i].sourceInfo.id};
            if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || std::wcscmp(name, source.viewGdiDeviceName)) continue;
            DISPLAYCONFIG_SDR_WHITE_LEVEL white {};
            white.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL, sizeof(white), paths[i].targetInfo.adapterId, paths[i].targetInfo.id};
            if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel)
                return white.SDRWhiteLevel / 1000.0f;
        }
        break;
    }
    return 0; // Caller retains last known value, initially the Windows 80-nit baseline.
}

CAP_HDR *cap_hdr_create(ID3D11Device *device, IDXGIOutput *output)
{
    auto *p = new (std::nothrow) CAP_HDR;
    if (!p) return nullptr;
    p->device = device;
    device->GetImmediateContext(&p->context);
    if (output) output->QueryInterface(IID_PPV_ARGS(&p->output));
    DXGI_OUTPUT_DESC1 desc {};
    if (p->output && SUCCEEDED(p->output->GetDesc1(&desc))) {
        p->enabled = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        if (p->enabled) {
            float white = cap_hdr_white(desc.DeviceName);
            if (white > 0) p->white = white;
            core_log(1, "Host HDR -> SDR enabled (SDR white %.0f nits%s)", p->white * 80,
                white > 0 ? "" : ", fallback");
        }
    }
    p->checked = GetTickCount64();
    return p;
}

void cap_hdr_destroy(CAP_HDR **out)
{
    if (out) { delete *out; *out = nullptr; }
}

bool cap_hdr_enabled(CAP_HDR *p) { return p && p->enabled; }

bool cap_hdr_refresh(CAP_HDR *p)
{
    if (!p || !p->output || GetTickCount64() - p->checked < 1000) return true;
    p->checked = GetTickCount64();
    DXGI_OUTPUT_DESC1 desc {};
    if (FAILED(p->output->GetDesc1(&desc))) return false;
    if (p->enabled != (desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)) return false;
    if (p->enabled) {
        float white = cap_hdr_white(desc.DeviceName);
        if (white > 0) p->white = white;
    }
    return true;
}

HRESULT cap_hdr_render(CAP_HDR *p, ID3D11Texture2D *source, float white, ID3D11Texture2D **out, DXGI_MODE_ROTATION rotation)
{
    if (!out) return E_INVALIDARG;
    *out = nullptr;
    if (!p || !source || !std::isfinite(white) || white <= 0) return E_INVALIDARG;
    if (rotation < DXGI_MODE_ROTATION_IDENTITY || rotation > DXGI_MODE_ROTATION_ROTATE270) return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC desc {}; source->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || desc.SampleDesc.Count != 1 || desc.ArraySize != 1 || desc.MipLevels != 1)
        return E_INVALIDARG;
    HRESULT hr;
    if (!p->vs && FAILED(hr = p->device->CreateVertexShader(cap_hdr_vs, sizeof(cap_hdr_vs), nullptr, &p->vs))) return hr;
    if (!p->ps && FAILED(hr = p->device->CreatePixelShader(cap_hdr_ps, sizeof(cap_hdr_ps), nullptr, &p->ps))) return hr;
    if (!p->constants) {
        D3D11_BUFFER_DESC cb {}; cb.ByteWidth = 16; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(hr = p->device->CreateBuffer(&cb, nullptr, &p->constants))) return hr;
    }
    D3D11_TEXTURE2D_DESC old {}; if (p->input) p->input->GetDesc(&old);
    const bool swap = rotation == DXGI_MODE_ROTATION_ROTATE90 || rotation == DXGI_MODE_ROTATION_ROTATE270;
    const UINT width = swap ? desc.Height : desc.Width, height = swap ? desc.Width : desc.Height;
    if (!p->rtv || old.Width != desc.Width || old.Height != desc.Height || p->rotation != rotation) {
        p->srv.Reset(); p->rtv.Reset(); p->input.Reset(); p->target.Reset();
        desc.Usage = D3D11_USAGE_DEFAULT; desc.CPUAccessFlags = desc.MiscFlags = 0;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(hr = p->device->CreateTexture2D(&desc, nullptr, &p->input))) return hr;
        if (FAILED(hr = p->device->CreateShaderResourceView(p->input.Get(), nullptr, &p->srv))) return hr;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Width = width; desc.Height = height;
        desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
        if (FAILED(hr = p->device->CreateTexture2D(&desc, nullptr, &p->target))) return hr;
        if (FAILED(hr = p->device->CreateRenderTargetView(p->target.Get(), nullptr, &p->rtv))) return hr;
        p->rotation = rotation;
    }
    auto *c = p->context.Get();
    // Duplication textures need not have shader-resource binding; copy entirely on GPU.
    c->CopyResource(p->input.Get(), source);
    float params[4] = {white, float(rotation), 0, 0}; c->UpdateSubresource(p->constants.Get(), 0, nullptr, params, 0, 0);
    D3D11_VIEWPORT viewport = {0, 0, float(width), float(height), 0, 1};
    c->IASetInputLayout(nullptr); c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(p->vs.Get(), nullptr, 0); c->PSSetShader(p->ps.Get(), nullptr, 0);
    c->GSSetShader(nullptr, nullptr, 0); c->HSSetShader(nullptr, nullptr, 0); c->DSSetShader(nullptr, nullptr, 0);
    c->RSSetState(nullptr); c->RSSetViewports(1, &viewport);
    c->OMSetBlendState(nullptr, nullptr, 0xffffffff); c->OMSetDepthStencilState(nullptr, 0);
    c->PSSetConstantBuffers(0, 1, p->constants.GetAddressOf());
    c->PSSetShaderResources(0, 1, p->srv.GetAddressOf());
    c->OMSetRenderTargets(1, p->rtv.GetAddressOf(), nullptr); c->Draw(3, 0);
    ID3D11ShaderResourceView *none = nullptr; c->PSSetShaderResources(0, 1, &none);
    c->OMSetRenderTargets(0, nullptr, nullptr);
    hr = p->device->GetDeviceRemovedReason();
    if (FAILED(hr)) return hr;
    *out = p->target.Get(); (*out)->AddRef();
    return S_OK;
}

HRESULT cap_hdr_convert(CAP_HDR *p, ID3D11Texture2D **texture, DXGI_MODE_ROTATION rotation)
{
    if (!p || !texture || !*texture) return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC desc {}; (*texture)->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
        if (p->enabled && !p->warned) {
            core_log(0, "Host HDR -> SDR: rejecting non-FP16 capture; clipped SDR data cannot be tone mapped as HDR");
            p->warned = true;
        }
        return p->enabled ? DXGI_ERROR_UNSUPPORTED : S_OK;
    }
    ID3D11Texture2D *converted = nullptr;
    HRESULT hr = cap_hdr_render(p, *texture, p->enabled ? p->white : 1.0f, &converted, rotation);
    if (SUCCEEDED(hr)) { (*texture)->Release(); *texture = converted; }
    return hr;
}
