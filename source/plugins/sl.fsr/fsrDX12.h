#pragma once

// FidelityFX's DX12 backend pieces for sl.fsr (fsrDX12.cpp): the backend's context description for a D3D12 device, and a D3D12
// resource's FfxApiResource in the state it is in.

#include <cstdint>

#include "ffx_api/ffx_api.h"
#include "ffx_api/ffx_api_types.h"

namespace sl::fsr
{
//! The DX12 backend's description for a_device (an ID3D12Device*), for ffxCreateContextDescUpscale's chain.
ffxCreateContextDescHeader* backendDescDX12(void* device);

//! a_resource (an ID3D12Resource*, or null) as FFX takes it, in a_state (an FfxApiResourceState).
FfxApiResource resourceDX12(void* resource, uint32_t state);
}
