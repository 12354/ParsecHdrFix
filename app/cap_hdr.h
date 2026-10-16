// Local host extension: linear scRGB desktop capture to SDR, before encoding.
#pragma once
#include <d3d11.h>
#include <dxgi1_6.h>

struct CAP_HDR;
// target: B8G8R8A8_UNORM or R10G10B10A2_UNORM, both sRGB-encoded SDR.
CAP_HDR *cap_hdr_create(ID3D11Device *device, IDXGIOutput *output, DXGI_FORMAT target = DXGI_FORMAT_B8G8R8A8_UNORM);
void cap_hdr_destroy(CAP_HDR **ctx);
bool cap_hdr_enabled(CAP_HDR *ctx);
// Refresh white level; false requests duplication recreation after an HDR toggle.
bool cap_hdr_refresh(CAP_HDR *ctx);
// Tone maps an FP16 source (or reformats a B8G8R8A8 one) straight into the caller's
// render target, which must be the source's size after rotation. Sources with
// shader-resource binding are read in place, others are copied first.
HRESULT cap_hdr_draw(CAP_HDR *ctx, ID3D11Texture2D *source, ID3D11RenderTargetView *target,
    DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY);
// Also used by the GPU fixture; white is in scRGB units (1 = 80 nits).
// A B8G8R8A8_UNORM source is already SDR and is only reformatted to the target.
HRESULT cap_hdr_render(CAP_HDR *ctx, ID3D11Texture2D *source, float white, ID3D11Texture2D **out,
    DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY);
