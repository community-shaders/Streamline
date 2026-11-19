/*
 * Community Shaders sl.fsr_g plugin: the D3D12 backend.
 *
 * The host makes its swap chain through Streamline's proxy factory. While frame generation is enabled, the factory's
 * CreateSwapChainForHwnd hook makes FidelityFX's DX12 frame interpolation swap chain instead (FOR_HWND_DX12) with the base
 * factory and the native queue, and the proxy wraps it. So FFX's own presents stay outside the interposer, and the host's one
 * Present per frame still runs through the proxy (presentCommon). The host renders into FFX's replacement back buffers
 * (GetBuffer through the proxy) and paces on FFX's replacement waitable object.
 *
 * The prepare runs on the host's D3D12 list at the evaluate. Each present configures FFX with the frame prepared for it.
 * The swap chain is wrapped only when the host makes one: this backend never asks for a recreate.
 */

#include <cstdlib>
#include <cstring>
#include <string>

#include <d3d12.h>
#include <dxgi1_6.h>

#include "include/sl.h"
#include "include/sl_consts.h"
#include "source/core/sl.api/internal.h"
#include "source/core/sl.log/log.h"
#include "source/core/sl.param/parameters.h"
#include "source/platforms/sl.chi/compute.h"
#include "source/plugins/sl.common/commonInterface.h"

#include "ffx_api/ffx_api.h"
#include "ffx_api/ffx_api_loader.h"
#include "ffx_api/ffx_framegeneration.h"
#include "ffx_api/dx12/ffx_api_dx12.h"

#include "source/plugins/sl.fsr_g/fsrGDX12.h"

namespace sl
{
namespace fsr
{
namespace dx12
{
namespace
{
// Skyrim world units to meters, as the Vulkan backend's prepare uses.
constexpr float kSkyrimViewSpaceToMeters = 0.01428222656f;

struct State
{
    SharedState shared{};
    HMODULE module = nullptr;
    ffxFunctions api{};

    // The interpolation context, sized to the swap chain. Guarded by g_dispatchMutex.
    ffxContext fgContext = nullptr;
    uint32_t displayW = 0, displayH = 0;
    DXGI_FORMAT backBufferFormat = DXGI_FORMAT_UNKNOWN;
    bool contextHDR = false;

    // FFX's swap chain context and its swap chain. The context holds a reference, the proxy holds the one creation gave.
    // Guarded by the shared frameMutex.
    ffxContext swapchainContext = nullptr;
    IDXGISwapChain4* swapchain = nullptr;

    int64_t lastPrepareQpc = 0;

    // The present in flight configured a generated frame. Set and read on the presenting thread.
    bool presentInFlight = false;
    bool presentGenerates = false;
    std::atomic<bool> generatedDispatchSucceeded{ false };
};

State g;

// Prepare, interpolation and configure all touch the interpolation context's state. Taken after frameMutex.
std::mutex g_dispatchMutex;

bool faulted()
{
    return g.shared.dispatchFaulted && g.shared.dispatchFaulted->load(std::memory_order_acquire);
}

void setFaulted()
{
    if (g.shared.dispatchFaulted)
        g.shared.dispatchFaulted->store(true, std::memory_order_release);
}

// SEH guards are leaf functions, so __try has no objects to unwind.
ffxReturnCode_t createContextSEH(ffxContext* context, ffxCreateContextDescHeader* desc)
{
    __try { return g.api.CreateContext(context, desc, nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { setFaulted(); return FFX_API_RETURN_ERROR; }
}
ffxReturnCode_t destroyContextSEH(ffxContext* context)
{
    __try { return g.api.DestroyContext(context, nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { setFaulted(); return FFX_API_RETURN_ERROR; }
}
ffxReturnCode_t configureSEH(ffxContext* context, const ffxConfigureDescHeader* desc)
{
    __try { return g.api.Configure(context, desc); }
    __except (EXCEPTION_EXECUTE_HANDLER) { setFaulted(); return FFX_API_RETURN_ERROR; }
}
ffxReturnCode_t dispatchSEH(ffxContext* context, const ffxDispatchDescHeader* desc)
{
    __try { return g.api.Dispatch(context, desc); }
    __except (EXCEPTION_EXECUTE_HANDLER) { setFaulted(); return FFX_API_RETURN_ERROR; }
}

void ffxApiLogMessage(uint32_t type, const wchar_t* message)
{
    if (!message)
        return;
    char buf[1024] = {};
    size_t conv = 0;
    wcstombs_s(&conv, buf, sizeof(buf), message, _TRUNCATE);
    if (type == FFX_API_MESSAGE_TYPE_ERROR) {
        SL_LOG_ERROR("sl.fsr_g: [FFX DX12] %s", buf);
    } else {
        SL_LOG_WARN("sl.fsr_g: [FFX DX12] %s", buf);
    }
}

std::wstring pluginDir()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&pluginDir), &self);
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p = path;
    auto slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

// A D3D12 resource state as FFX names it. The host's frame generation inputs are in the common state.
uint32_t ffxState(uint32_t a_d3d12State)
{
    if (a_d3d12State == D3D12_RESOURCE_STATE_COMMON)
        return FFX_API_RESOURCE_STATE_COMMON;
    if (a_d3d12State & D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        return FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
    if (a_d3d12State & D3D12_RESOURCE_STATE_COPY_DEST)
        return FFX_API_RESOURCE_STATE_COPY_DEST;
    if (a_d3d12State & D3D12_RESOURCE_STATE_RENDER_TARGET)
        return FFX_API_RESOURCE_STATE_RENDER_TARGET;
    const bool pixel = (a_d3d12State & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) != 0;
    const bool compute = (a_d3d12State & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0;
    if (pixel && compute)
        return FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    if (pixel)
        return FFX_API_RESOURCE_STATE_PIXEL_READ;
    if (compute)
        return FFX_API_RESOURCE_STATE_COMPUTE_READ;
    if (a_d3d12State & D3D12_RESOURCE_STATE_COPY_SOURCE)
        return FFX_API_RESOURCE_STATE_COPY_SRC;
    return FFX_API_RESOURCE_STATE_COMMON;
}

void readOptions(bool& a_enabled, bool& a_hdr, uint32_t& a_debugFlags, bool& a_onlyGenerated)
{
    std::lock_guard<std::mutex> lock(*g.shared.optionsMutex);
    a_enabled = *g.shared.fgEnabled;
    a_hdr = *g.shared.fgColorHDR;
    a_debugFlags = *g.shared.fgDebugFlags;
    a_onlyGenerated = *g.shared.fgOnlyPresentGenerated;
}

// FFX's swap chain calls this per generated frame, inside the host's present.
ffxReturnCode_t frameGenDispatchCallback(ffxDispatchDescFrameGeneration* params, void*)
{
    std::lock_guard<std::mutex> lock(g_dispatchMutex);
    if (!g.fgContext)
        return FFX_API_RETURN_ERROR;
    const ffxReturnCode_t result = dispatchSEH(&g.fgContext, &params->header);
    g.generatedDispatchSucceeded.store(result == FFX_API_RETURN_OK, std::memory_order_release);
    return result;
}

// Caller holds g_dispatchMutex.
bool destroyFgContextLocked()
{
    if (!g.fgContext)
        return true;
    const ffxReturnCode_t rc = destroyContextSEH(&g.fgContext);
    g.fgContext = nullptr;
    if (rc != FFX_API_RETURN_OK) {
        setFaulted();
        SL_LOG_ERROR("sl.fsr_g: D3D12 FG context DestroyContext failed 0x%08X", (uint32_t)rc);
        return false;
    }
    return true;
}

// The interpolation context for a swap chain of this size and format, made again when they or the HDR option change.
bool ensureFgContext(uint32_t a_width, uint32_t a_height, DXGI_FORMAT a_format)
{
    bool enabled = false, hdr = false, onlyGenerated = false;
    uint32_t debugFlags = 0;
    readOptions(enabled, hdr, debugFlags, onlyGenerated);

    std::lock_guard<std::mutex> lock(g_dispatchMutex);
    if (g.fgContext && g.displayW == a_width && g.displayH == a_height && g.backBufferFormat == a_format && g.contextHDR == hdr)
        return true;
    if (!destroyFgContextLocked())
        return false;

    chi::Device device{};
    g.shared.compute->getDevice(device);

    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.header.pNext = nullptr;
    backend.device = static_cast<ID3D12Device*>(device);

    ffxCreateContextDescFrameGeneration desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    desc.header.pNext = &backend.header;
    // Skyrim's depth is not reversed. Generation runs on the game queue (no async workloads).
    desc.flags = hdr ? FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE : 0;
    desc.displaySize = { a_width, a_height };
    desc.maxRenderSize = { a_width, a_height };
    desc.backBufferFormat = ffxApiGetSurfaceFormatDX12(a_format);

    ffxConfigureDescGlobalDebug1 debug{};
    debug.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1;
    debug.fpMessage = &ffxApiLogMessage;
    debug.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
    configureSEH(nullptr, &debug.header);

    ffxContext context = nullptr;
    const ffxReturnCode_t rc = createContextSEH(&context, &desc.header);
    if (rc != FFX_API_RETURN_OK || !context) {
        setFaulted();
        SL_LOG_ERROR("sl.fsr_g: D3D12 ffxCreateContext(FG) failed 0x%08X", (uint32_t)rc);
        return false;
    }
    g.fgContext = context;
    g.displayW = a_width;
    g.displayH = a_height;
    g.backBufferFormat = a_format;
    g.contextHDR = hdr;
    SL_LOG_INFO("sl.fsr_g: D3D12 FG context created (%ux%u, DXGI format %d, HDR %d)", a_width, a_height, (int)a_format, (int)hdr);
    return true;
}

bool applyPacingTuning()
{
    FfxApiSwapchainFramePacingTuning pacing{};
    {
        std::lock_guard<std::mutex> lock(*g.shared.optionsMutex);
        pacing.safetyMarginInMs = *g.shared.fgPacingSafetyMs;
        pacing.varianceFactor = *g.shared.fgPacingVariance;
    }
    // As the Vulkan backend: FFX's documented hybrid spin, not zero.
    pacing.allowHybridSpin = true;
    pacing.hybridSpinTime = 2;
    pacing.allowWaitForSingleObjectOnFence = true;
    ffxConfigureDescFrameGenerationSwapChainKeyValueDX12 kv{};
    kv.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12;
    kv.key = FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING;
    kv.ptr = &pacing;
    const ffxReturnCode_t rc = configureSEH(&g.swapchainContext, &kv.header);
    SL_LOG_INFO("sl.fsr_g: D3D12 FG frame-pacing tuning (rc=0x%08X safety=%.3fms variance=%.3f)",
        (uint32_t)rc, pacing.safetyMarginInMs, pacing.varianceFactor);
    return rc == FFX_API_RETURN_OK;
}

// Unlinks the interpolation context, waits for FFX's presents and destroys the swap chain context. Caller holds frameMutex.
bool teardownSwapchainLocked()
{
    if (!g.swapchainContext)
        return true;
    *g.shared.pendingFrame = {};
    {
        std::lock_guard<std::mutex> lock(g_dispatchMutex);
        if (g.fgContext) {
            ffxConfigureDescFrameGeneration cfg{};
            cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            cfg.swapChain = g.swapchain;
            cfg.frameGenerationEnabled = false;
            cfg.frameID = *g.shared.frameID;
            if (const ffxReturnCode_t rc = configureSEH(&g.fgContext, &cfg.header); rc != FFX_API_RETURN_OK) {
                setFaulted();
                SL_LOG_ERROR("sl.fsr_g: D3D12 FG swapchain unlink failed 0x%08X", (uint32_t)rc);
                return false;
            }
        }
    }
    ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 wait{};
    wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
    if (const ffxReturnCode_t rc = dispatchSEH(&g.swapchainContext, &wait.header); rc != FFX_API_RETURN_OK) {
        setFaulted();
        SL_LOG_ERROR("sl.fsr_g: D3D12 FG WaitForPresents failed 0x%08X", (uint32_t)rc);
        return false;
    }
    const ffxReturnCode_t rc = destroyContextSEH(&g.swapchainContext);
    g.swapchainContext = nullptr;
    g.swapchain = nullptr;
    g.presentInFlight = false;
    g.shared.lastPresentedFrameCount->store(1, std::memory_order_release);
    if (rc != FFX_API_RETURN_OK) {
        setFaulted();
        SL_LOG_ERROR("sl.fsr_g: D3D12 FG swapchain DestroyContext failed 0x%08X", (uint32_t)rc);
        return false;
    }
    SL_LOG_INFO("sl.fsr_g: D3D12 FFX FG swapchain torn down");
    return true;
}
}

bool startup(const SharedState& a_shared)
{
    g.shared = a_shared;
    if (g.module)
        return true;
    const std::wstring dll = pluginDir() + L"\\amd_fidelityfx_dx12.dll";
    g.module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g.module) {
        SL_LOG_ERROR("sl.fsr_g: amd_fidelityfx_dx12.dll not found next to the plugin");
        return false;
    }
    ffxLoadFunctions(&g.api, g.module);
    if (!g.api.CreateContext || !g.api.DestroyContext || !g.api.Configure || !g.api.Dispatch || !g.api.Query) {
        SL_LOG_ERROR("sl.fsr_g: amd_fidelityfx_dx12.dll lacks FFX-API entry points");
        FreeLibrary(g.module);
        g.module = nullptr;
        return false;
    }
    SL_LOG_INFO("sl.fsr_g: loaded amd_fidelityfx_dx12.dll");
    return true;
}

bool shutdown()
{
    bool ok = true;
    if (g.shared.frameMutex) {
        std::lock_guard<std::mutex> frameLock(*g.shared.frameMutex);
        ok = teardownSwapchainLocked();
    }
    {
        std::lock_guard<std::mutex> lock(g_dispatchMutex);
        ok = destroyFgContextLocked() && ok;
    }
    if (!ok) {
        SL_LOG_ERROR("sl.fsr_g: retaining FidelityFX DX12 after a failed teardown");
        return false;
    }
    if (g.module)
        FreeLibrary(g.module);
    g.module = nullptr;
    g.api = {};
    return true;
}

bool completeSwapchainTeardown(bool a_releaseFeatureContext)
{
    {
        std::lock_guard<std::mutex> frameLock(*g.shared.frameMutex);
        if (!teardownSwapchainLocked())
            return false;
    }
    if (!a_releaseFeatureContext)
        return !faulted();
    std::lock_guard<std::mutex> lock(g_dispatchMutex);
    return destroyFgContextLocked();
}

Result endEvaluation(void* a_cmdList, const common::EventData& evd, const BaseStructure** inputs, uint32_t numInputs)
{
    // Any failure leaves no prepared frame behind.
    struct Guard
    {
        bool accepted = false;
        ~Guard()
        {
            if (!accepted) {
                std::lock_guard<std::mutex> lock(*g.shared.frameMutex);
                *g.shared.pendingFrame = {};
            }
        }
    } guard;

    if (faulted() || !g.module)
        return Result::eErrorNotInitialized;

    Constants* consts{};
    if (!common::getConsts(evd, &consts))
        return Result::eErrorMissingConstants;

    CommonResource depth{}, mvec{}, hudless{};
    getTaggedResource(kBufferTypeDepth, depth, evd.frame, evd.id, false, inputs, numInputs);
    getTaggedResource(kBufferTypeMotionVectors, mvec, evd.frame, evd.id, false, inputs, numInputs);
    getTaggedResource(kBufferTypeHUDLessColor, hudless, evd.frame, evd.id, true, inputs, numInputs);
    if (!depth || !mvec || !hudless)
        return Result::eErrorMissingInputParameter;
    const chi::Resource depthRes = depth;
    const chi::Resource mvecRes = mvec;
    const chi::Resource hudlessRes = hudless;
    if (!depthRes || !depthRes->native || !mvecRes || !mvecRes->native || !hudlessRes || !hudlessRes->native)
        return Result::eErrorMissingInputParameter;

    const auto& extent = depth.getExtent();
    const uint32_t renderW = extent.width;
    const uint32_t renderH = extent.height;
    if (!renderW || !renderH)
        return Result::eErrorMissingInputParameter;

    bool enabled = false, hdr = false, onlyGenerated = false;
    uint32_t debugFlags = 0;
    readOptions(enabled, hdr, debugFlags, onlyGenerated);
    if (!enabled)
        return Result::eErrorInvalidState;

    auto* hudlessResource = static_cast<ID3D12Resource*>(hudlessRes->native);
    const DXGI_FORMAT hudlessFormat = hudlessResource->GetDesc().Format;

    std::lock_guard<std::mutex> frameLock(*g.shared.frameMutex);
    *g.shared.pendingFrame = {};
    // No wrapped swap chain yet: the frame presents as it is.
    if (!g.swapchainContext)
        return Result::eErrorInvalidState;
    if (hudlessFormat != g.backBufferFormat) {
        SL_LOG_WARN("sl.fsr_g: D3D12 HUD-less format %d does not match the back buffer format %d", (int)hudlessFormat, (int)g.backBufferFormat);
        return Result::eErrorComputeFailed;
    }

    ffxDispatchDescFrameGenerationPrepare prep{};
    prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
    prep.commandList = a_cmdList;
    prep.renderSize = { renderW, renderH };
    // The host already negates the jitter for FFX.
    prep.jitterOffset = { consts->jitterOffset.x, consts->jitterOffset.y };
    prep.motionVectorScale = { (float)renderW, (float)renderH };
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float deltaMs = 16.6f;
        if (g.lastPrepareQpc) {
            LARGE_INTEGER freq;
            QueryPerformanceFrequency(&freq);
            const double ms = double(now.QuadPart - g.lastPrepareQpc) * 1000.0 / double(freq.QuadPart);
            deltaMs = (float)std::min(std::max(ms, 1.0), 100.0);
        }
        g.lastPrepareQpc = now.QuadPart;
        prep.frameTimeDelta = deltaMs;
    }
    prep.cameraNear = consts->cameraNear;
    prep.cameraFar = consts->cameraFar;
    prep.cameraFovAngleVertical = consts->cameraFOV;
    prep.viewSpaceToMetersFactor = kSkyrimViewSpaceToMeters;
    prep.depth = ffxApiGetResourceDX12(static_cast<ID3D12Resource*>(depthRes->native), ffxState(depthRes->state));
    prep.motionVectors = ffxApiGetResourceDX12(static_cast<ID3D12Resource*>(mvecRes->native), ffxState(mvecRes->state));
    // FFX reconstructs the motion vectors on depth's grid, so both carry depth's size.
    prep.motionVectors.description.width = prep.depth.description.width;
    prep.motionVectors.description.height = prep.depth.description.height;
    prep.frameID = *g.shared.frameID;

    fsr::FrameGenerationFrame frame{};
    frame.frameID = prep.frameID;
    frame.hudlessResource = ffxApiGetResourceDX12(hudlessResource, ffxState(hudlessRes->state));
    frame.haveHudless = true;
    frame.hudlessCompatible = true;

    ffxReturnCode_t rc = FFX_API_RETURN_ERROR;
    {
        std::lock_guard<std::mutex> lock(g_dispatchMutex);
        if (g.fgContext)
            rc = dispatchSEH(&g.fgContext, &prep.header);
    }
    if (rc != FFX_API_RETURN_OK) {
        SL_LOG_ERROR("sl.fsr_g: D3D12 FrameGenerationPrepare failed 0x%08X", (uint32_t)rc);
        return Result::eErrorComputeFailed;
    }
    frame.prepared = true;
    *g.shared.pendingFrame = frame;
    guard.accepted = true;
    return Result::eOk;
}
}
}

HRESULT slHookDX12CreateSwapChainForHwnd(IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGIOutput* /*pRestrictToOutput*/, IDXGISwapChain1** ppSwapChain, bool& Skip)
{
    using namespace fsr::dx12;
    if (!g.module || !pDesc || !ppSwapChain || !pDevice || !pFactory)
        return S_OK;
    bool enabled = false, hdr = false, onlyGenerated = false;
    uint32_t debugFlags = 0;
    readOptions(enabled, hdr, debugFlags, onlyGenerated);
    SL_LOG_INFO("sl.fsr_g: D3D12 CreateSwapChainForHwnd (fgEnabled=%d) - %s", (int)enabled, enabled ? "will wrap" : "pass-through");
    if (!enabled)
        return S_OK;
    if (faulted())
        return DXGI_ERROR_DEVICE_REMOVED;
    {
        std::lock_guard<std::mutex> frameLock(*g.shared.frameMutex);
        // DXGI allows one flip-model swap chain per window. The host destroys the old one first.
        if (g.swapchainContext) {
            SL_LOG_ERROR("sl.fsr_g: D3D12 swap chain created while the wrapped one still exists");
            return DXGI_ERROR_INVALID_CALL;
        }
    }

    // FFX submits to the native queue, not the proxy, so its work stays outside the interposer.
    ID3D12CommandQueue* queue = nullptr;
    {
        IUnknown* base = nullptr;
        if (SUCCEEDED(pDevice->QueryInterface(__uuidof(StreamlineRetrieveBaseInterface), reinterpret_cast<void**>(&base))) && base) {
            base->QueryInterface(IID_PPV_ARGS(&queue));
            base->Release();
        } else {
            pDevice->QueryInterface(IID_PPV_ARGS(&queue));
        }
    }
    if (!queue) {
        SL_LOG_ERROR("sl.fsr_g: D3D12 swap chain's device is not a command queue");
        return DXGI_ERROR_INVALID_CALL;
    }

    if (!ensureFgContext(pDesc->Width, pDesc->Height, pDesc->Format)) {
        queue->Release();
        return E_FAIL;
    }

    DXGI_SWAP_CHAIN_DESC1 desc = *pDesc;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen{};
    if (pFullscreenDesc)
        fullscreen = *pFullscreenDesc;
    IDXGISwapChain4* swapchain = nullptr;
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    create.swapchain = &swapchain;
    create.hwnd = hWnd;
    create.desc = &desc;
    create.fullscreenDesc = pFullscreenDesc ? &fullscreen : nullptr;
    create.dxgiFactory = pFactory;
    create.gameQueue = queue;
    ffxContext context = nullptr;
    const ffxReturnCode_t rc = createContextSEH(&context, &create.header);
    queue->Release();
    if (rc != FFX_API_RETURN_OK || !context || !swapchain) {
        setFaulted();
        SL_LOG_ERROR("sl.fsr_g: D3D12 FG swapchain CreateContext failed 0x%08X", (uint32_t)rc);
        if (context)
            destroyContextSEH(&context);
        return E_FAIL;
    }

    {
        std::lock_guard<std::mutex> frameLock(*g.shared.frameMutex);
        g.swapchainContext = context;
        g.swapchain = swapchain;
        g.presentInFlight = false;
        *g.shared.frameID = 0;
        *g.shared.pendingFrame = {};
    }
    g.shared.lastPresentedFrameCount->store(1, std::memory_order_release);

    // Linked with generation off. A present turns it on only for a prepared frame.
    ffxConfigureDescFrameGeneration cfg{};
    cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.swapChain = swapchain;
    cfg.frameGenerationCallback = &frameGenDispatchCallback;
    cfg.frameGenerationEnabled = false;
    cfg.allowAsyncWorkloads = false;
    cfg.generationRect = { 0, 0, (int32_t)desc.Width, (int32_t)desc.Height };
    cfg.frameID = 0;
    ffxReturnCode_t linkRc = FFX_API_RETURN_ERROR;
    {
        std::lock_guard<std::mutex> lock(g_dispatchMutex);
        if (g.fgContext)
            linkRc = configureSEH(&g.fgContext, &cfg.header);
    }
    if (linkRc != FFX_API_RETURN_OK || !applyPacingTuning()) {
        setFaulted();
        SL_LOG_ERROR("sl.fsr_g: D3D12 FG swapchain link failed 0x%08X", (uint32_t)linkRc);
        swapchain->Release();
        std::lock_guard<std::mutex> frameLock(*g.shared.frameMutex);
        teardownSwapchainLocked();
        return E_FAIL;
    }
    g.shared.fgPacingDirty->store(false, std::memory_order_release);

    // The proxy takes the reference creation gave.
    *ppSwapChain = swapchain;
    Skip = true;
    SL_LOG_INFO("sl.fsr_g: D3D12 FFX FG swapchain created (%ux%u, DXGI format %d, %u buffers)", desc.Width, desc.Height, (int)desc.Format, desc.BufferCount);
    return S_OK;
}

void slHookDX12SwapchainDestroyed(IDXGISwapChain* SwapChain)
{
    using namespace fsr::dx12;
    if (!g.shared.frameMutex)
        return;
    std::lock_guard<std::mutex> frameLock(*g.shared.frameMutex);
    if (!SwapChain || SwapChain != static_cast<IDXGISwapChain*>(g.swapchain))
        return;
    if (!teardownSwapchainLocked())
        SL_LOG_ERROR("sl.fsr_g: D3D12 FG swapchain teardown failed at its destruction");
}

HRESULT slHookDX12ResizeBuffers(IDXGISwapChain* SwapChain, UINT, UINT, UINT, DXGI_FORMAT, UINT&, bool&)
{
    using namespace fsr::dx12;
    if (!g.shared.frameMutex)
        return S_OK;
    std::lock_guard<std::mutex> frameLock(*g.shared.frameMutex);
    if (!SwapChain || SwapChain != static_cast<IDXGISwapChain*>(g.swapchain))
        return S_OK;
    // The interpolation context is sized to the swap chain: the next present makes it again at the new size.
    *g.shared.pendingFrame = {};
    std::lock_guard<std::mutex> lock(g_dispatchMutex);
    if (g.fgContext) {
        ffxConfigureDescFrameGeneration cfg{};
        cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        cfg.swapChain = g.swapchain;
        cfg.frameGenerationEnabled = false;
        cfg.frameID = *g.shared.frameID;
        if (configureSEH(&g.fgContext, &cfg.header) != FFX_API_RETURN_OK) {
            setFaulted();
            return DXGI_ERROR_DEVICE_REMOVED;
        }
    }
    ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 wait{};
    wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
    if (dispatchSEH(&g.swapchainContext, &wait.header) != FFX_API_RETURN_OK) {
        setFaulted();
        return DXGI_ERROR_DEVICE_REMOVED;
    }
    if (!destroyFgContextLocked())
        return DXGI_ERROR_DEVICE_REMOVED;
    SL_LOG_INFO("sl.fsr_g: D3D12 FG context released for a swap chain resize");
    return S_OK;
}

HRESULT slHookDX12Present(IDXGISwapChain* SwapChain, UINT, UINT Flags, bool&)
{
    using namespace fsr::dx12;
    if (!g.shared.frameMutex || !SwapChain || (Flags & DXGI_PRESENT_TEST))
        return S_OK;
    {
        std::lock_guard<std::mutex> frameLock(*g.shared.frameMutex);
        if (SwapChain != static_cast<IDXGISwapChain*>(g.swapchain))
            return S_OK;
    }
    if (faulted())
        return DXGI_ERROR_DEVICE_REMOVED;

    // After a resize the context is made again at the swap chain's new size. An HDR change waits for the host's replacement.
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (FAILED(g.swapchain->GetDesc1(&desc)))
        return DXGI_ERROR_DEVICE_REMOVED;
    bool sized = false;
    {
        std::lock_guard<std::mutex> lock(g_dispatchMutex);
        sized = g.fgContext && g.displayW == desc.Width && g.displayH == desc.Height && g.backBufferFormat == desc.Format;
    }
    if (!sized && !ensureFgContext(desc.Width, desc.Height, desc.Format))
        return DXGI_ERROR_DEVICE_REMOVED;

    if (g.shared.fgPacingDirty->exchange(false, std::memory_order_acq_rel))
        applyPacingTuning();

    bool enabled = false, hdr = false, onlyGenerated = false;
    uint32_t debugFlags = 0;
    readOptions(enabled, hdr, debugFlags, onlyGenerated);

    // The host's contract: frame N's prepare is recorded after present N-1 and before present N. A frame prepared for another
    // frame ID is a host error, passed through and logged.
    fsr::FrameGenerationFrame frame{};
    uint64_t frameID = 0;
    uint64_t foreignFrameID = 0;
    bool foreign = false;
    {
        std::lock_guard<std::mutex> lock(*g.shared.frameMutex);
        frameID = (*g.shared.frameID)++;
        if (g.shared.pendingFrame->prepared && g.shared.pendingFrame->frameID == frameID) {
            frame = *g.shared.pendingFrame;
        } else if (g.shared.pendingFrame->prepared) {
            foreign = true;
            foreignFrameID = g.shared.pendingFrame->frameID;
        }
        *g.shared.pendingFrame = {};
    }
    if (foreign) {
        const uint64_t count = g.shared.foreignPreparedFrames->fetch_add(1, std::memory_order_acq_rel) + 1;
        if (count == 1 || count % 600 == 0)
            SL_LOG_ERROR("sl.fsr_g: present %llu found a frame prepared for frame %llu (host error, %llu so far); passed through",
                (unsigned long long)frameID, (unsigned long long)foreignFrameID, (unsigned long long)count);
    }

    ffxConfigureDescFrameGeneration cfg{};
    cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.swapChain = g.swapchain;
    cfg.frameGenerationCallback = &frameGenDispatchCallback;
    cfg.frameGenerationEnabled = enabled && frame.prepared;
    cfg.allowAsyncWorkloads = false;
    cfg.flags = debugFlags;
    cfg.onlyPresentGenerated = onlyGenerated;
    cfg.generationRect = { 0, 0, (int32_t)desc.Width, (int32_t)desc.Height };
    if (frame.haveHudless && frame.hudlessCompatible)
        cfg.HUDLessColor = frame.hudlessResource;
    cfg.frameID = frameID;
    ffxReturnCode_t rc = FFX_API_RETURN_ERROR;
    {
        std::lock_guard<std::mutex> lock(g_dispatchMutex);
        if (g.fgContext)
            rc = configureSEH(&g.fgContext, &cfg.header);
    }
    if (rc != FFX_API_RETURN_OK || faulted()) {
        setFaulted();
        SL_LOG_ERROR("sl.fsr_g: D3D12 present configure failed 0x%08X", (uint32_t)rc);
        return DXGI_ERROR_DEVICE_REMOVED;
    }
    g.generatedDispatchSucceeded.store(false, std::memory_order_release);
    g.presentGenerates = cfg.frameGenerationEnabled;
    g.presentInFlight = true;
    return S_OK;
}

HRESULT slHookDX12AfterPresent(UINT Flags)
{
    using namespace fsr::dx12;
    if ((Flags & DXGI_PRESENT_TEST) || !g.presentInFlight)
        return S_OK;
    g.presentInFlight = false;
    // FFX's interpolation dispatch ran inside that present, through the callback.
    const uint32_t presented = g.presentGenerates && g.generatedDispatchSucceeded.load(std::memory_order_acquire) ? 2u : 1u;
    g.shared.lastPresentedFrameCount->store(presented, std::memory_order_release);
    g.shared.totalPresentedFrames->fetch_add(presented, std::memory_order_release);
    return S_OK;
}
}
