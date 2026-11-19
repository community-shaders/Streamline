#pragma once

// sl.fsr_g's D3D12 backend (fsrGDX12.cpp): FidelityFX's DX12 frame interpolation swap chain, made in the host's
// CreateSwapChainForHwnd through Streamline's proxy factory, and the frame generation prepare on the host's D3D12 list.

#include <cstdint>

#include <dxgi1_4.h>

#include "source/plugins/sl.fsr_g/fsrGShared.h"

namespace sl
{
struct BaseStructure;
enum class Result;
namespace common
{
struct EventData;
}

namespace fsr
{
namespace dx12
{
//! Loads amd_fidelityfx_dx12.dll and keeps a_shared. False when the DLL or its entry points are missing.
bool startup(const SharedState& a_shared);

//! Tears down the swap chain context and the interpolation context, then unloads FidelityFX. False if a teardown failed.
bool shutdown();

//! The prepare for this frame, on a_cmdList (an ID3D12GraphicsCommandList*).
Result endEvaluation(void* a_cmdList, const common::EventData& a_evd, const BaseStructure** a_inputs, uint32_t a_numInputs);

//! The host's step between a swap chain's destruction and the next creation.
bool completeSwapchainTeardown(bool a_releaseFeatureContext);
}
}

// DXGI hooks, listed in fsr_g.json and exported by fsrGEntry.cpp.
HRESULT slHookDX12CreateSwapChainForHwnd(IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain, bool& Skip);
void slHookDX12SwapchainDestroyed(IDXGISwapChain* SwapChain);
HRESULT slHookDX12Present(IDXGISwapChain* SwapChain, UINT SyncInterval, UINT Flags, bool& Skip);
HRESULT slHookDX12AfterPresent(UINT Flags);
HRESULT slHookDX12ResizeBuffers(IDXGISwapChain* SwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT& SwapChainFlags, bool& Skip);
}
