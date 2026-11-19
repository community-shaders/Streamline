#pragma once

// What sl.fsr_g's entry (fsrGEntry.cpp, FidelityFX's Vulkan backend) shares with its D3D12 backend (fsrGDX12.cpp).
// ffx_api_dx12.h cannot be included beside ffx_api_vk.h, so the two backends meet only through this header.

#include <atomic>
#include <cstdint>
#include <mutex>

#include "ffx_api/ffx_api.h"
#include "ffx_api/ffx_api_types.h"

namespace sl
{
namespace chi
{
class ICompute;
}

namespace fsr
{
// One prepared frame, handed from the evaluate to the present that shows it.
struct FrameGenerationFrame
{
    FfxApiResource hudlessResource{};
    uint64_t frameID = 0;
    bool prepared = false;
    bool haveHudless = false;
    bool hudlessCompatible = false;
};

// The entry's state the D3D12 backend reads and writes. The options sit under optionsMutex, the prepared frame and the frame
// counter under frameMutex, as in the Vulkan backend.
struct SharedState
{
    chi::ICompute* compute = nullptr;

    std::mutex* optionsMutex = nullptr;
    const bool* fgEnabled = nullptr;
    const bool* fgColorHDR = nullptr;
    const uint32_t* fgDebugFlags = nullptr;
    const bool* fgOnlyPresentGenerated = nullptr;
    const float* fgPacingSafetyMs = nullptr;
    const float* fgPacingVariance = nullptr;
    std::atomic<bool>* fgPacingDirty = nullptr;

    std::mutex* frameMutex = nullptr;
    FrameGenerationFrame* pendingFrame = nullptr;
    uint64_t* frameID = nullptr;

    std::atomic<bool>* dispatchFaulted = nullptr;
    std::atomic<uint32_t>* lastPresentedFrameCount = nullptr;
    std::atomic<uint64_t>* totalPresentedFrames = nullptr;
    std::atomic<uint64_t>* foreignPreparedFrames = nullptr;
};
}
}
