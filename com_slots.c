#define CINTERFACE
#include <dxgi1_6.h>
#include <stddef.h>
#define SLOT(type, member) (unsigned)(offsetof(type, member) / sizeof(void *))
const unsigned slot_factory_enum = SLOT(IDXGIFactoryVtbl, EnumAdapters);
const unsigned slot_factory_enum1 = SLOT(IDXGIFactory1Vtbl, EnumAdapters1);
const unsigned slot_factory_luid = SLOT(IDXGIFactory4Vtbl, EnumAdapterByLuid);
const unsigned slot_factory_warp = SLOT(IDXGIFactory4Vtbl, EnumWarpAdapter);
const unsigned slot_factory_gpu = SLOT(IDXGIFactory6Vtbl, EnumAdapterByGpuPreference);
const unsigned slot_adapter_outputs = SLOT(IDXGIAdapterVtbl, EnumOutputs);
const unsigned slot_output_duplicate = SLOT(IDXGIOutput1Vtbl, DuplicateOutput);
const unsigned slot_output_duplicate1 = SLOT(IDXGIOutput5Vtbl, DuplicateOutput1);
