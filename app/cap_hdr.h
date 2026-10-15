// Local host extension: linear scRGB desktop capture to SDR, before encoding.
#pragma once
#include <d3d11.h>
#include <dxgi1_6.h>

struct CAP_HDR;
CAP_HDR *cap_hdr_create(ID3D11Device *device, IDXGIOutput *output);
void cap_hdr_destroy(CAP_HDR **ctx);
bool cap_hdr_enabled(CAP_HDR *ctx);
// Refresh white level; false requests duplication recreation after an HDR toggle.
bool cap_hdr_refresh(CAP_HDR *ctx);
// Replaces owned FP16 texture with SDR; non-FP16 is rejected on HDR displays.
HRESULT cap_hdr_convert(CAP_HDR *ctx, ID3D11Texture2D **texture, DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY);
// Also used by the GPU fixture; white is in scRGB units (1 = 80 nits).
HRESULT cap_hdr_render(CAP_HDR *ctx, ID3D11Texture2D *source, float white, ID3D11Texture2D **out,
    DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY);
