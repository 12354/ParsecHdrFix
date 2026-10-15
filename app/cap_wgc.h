// Local HDR capture extension: Windows Graphics Capture, FP16 scRGB only.
#pragma once
#include <d3d11.h>
struct CAP_WGC;
HRESULT cap_wgc_create(CAP_WGC **out, ID3D11Device *device, HMONITOR monitor, bool require_borderless = false);
void cap_wgc_destroy(CAP_WGC **ctx);
// Owned texture reference, valid until the next acquisition/destroy. No SDR fallback.
HRESULT cap_wgc_acquire(CAP_WGC *ctx, UINT timeout, ID3D11Texture2D **out);
// True only after Windows grants permission and the session accepts borderless.
bool cap_wgc_borderless(CAP_WGC *ctx);
