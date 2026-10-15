#pragma once
#include <stdint.h>
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d11.h>
// ABI of our optional diagnostics, independent of any application.
struct HDR_SHIM_STATS {
    ULONG size, version;
    LONG factories, duplication_calls, hdr_sessions, fallbacks, frames;
};
extern volatile LONG hdr_factories, hdr_duplication_calls, hdr_sessions, hdr_fallbacks, hdr_frames;
void api_bootstrap(HMODULE self);
void api_hook_factory(IUnknown *factory);
HRESULT hdr_duplication_create(IDXGIOutput *output, IUnknown *device, IDXGIOutputDuplication **out);
void shim_pin();
// SDK-derived offsets, compiled as C so C++ does not guess COM slots.
extern "C" {
extern const unsigned slot_factory_enum, slot_factory_enum1, slot_factory_luid, slot_factory_warp, slot_factory_gpu;
extern const unsigned slot_adapter_outputs, slot_output_duplicate, slot_output_duplicate1;
}
