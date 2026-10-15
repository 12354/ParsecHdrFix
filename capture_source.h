#pragma once
#include <d3d11.h>
struct SHIM_CAPTURE;
HRESULT shim_capture_create(SHIM_CAPTURE **out,ID3D11Device *device,HMONITOR monitor);
HRESULT shim_capture_acquire(SHIM_CAPTURE *capture,UINT timeout,ID3D11Texture2D **out);
void shim_capture_destroy(SHIM_CAPTURE **capture);
