/*
* Community Shaders sl.fsr plugin: FidelityFX's DX12 backend pieces. ffx_api_dx12.h cannot be included beside ffx_api_vk.h (both
* declare the frame-generation swap chain keys), so the D3D12 descriptions are made here and fsrEntry.cpp names them through
* fsrDX12.h.
*/

#include <d3d12.h>

#include "ffx_api/ffx_api.h"
#include "ffx_api/dx12/ffx_api_dx12.h"

#include "source/plugins/sl.fsr/fsrDX12.h"

namespace sl::fsr
{
ffxCreateContextDescHeader* backendDescDX12(void* device)
{
    // One upscale context at a time is made, on the render thread: a static description is enough.
    static ffxCreateBackendDX12Desc desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    desc.header.pNext = nullptr;
    desc.device = static_cast<ID3D12Device*>(device);
    return &desc.header;
}

FfxApiResource resourceDX12(void* resource, uint32_t state)
{
    return ffxApiGetResourceDX12(static_cast<ID3D12Resource*>(resource), state);
}
}
