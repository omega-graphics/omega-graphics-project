#include "omegaGTE/GTEDevice.h"
#include <atomic>
#include <iostream>
#include <cstdlib>
#include <string>

_NAMESPACE_BEGIN_

namespace {
#if defined(OMEGAGTE_DEBUG)
    constexpr bool kDebugLayerDefault = true;
#else
    constexpr bool kDebugLayerDefault = false;
#endif

    std::atomic<bool> g_debugLayerEnabled{kDebugLayerDefault};
    std::atomic<bool> g_gpuBasedValidation{false};
    std::atomic<bool> g_captureOnInit{false};

    // Written once in resolveDebugFlags() and only read afterward (by the
    // Metal engine constructor on the same Init() thread). The flags are
    // frozen for the process lifetime, so a plain string is sufficient —
    // no concurrent access in normal use.
    std::string g_captureOutputPath;

    void resolveDebugFlags(const GTEInitOptions &opts){
        bool enabled = kDebugLayerDefault;
        switch(opts.debugLayer){
            case GTEInitOptions::DebugLayer::Enabled:  enabled = true;  break;
            case GTEInitOptions::DebugLayer::Disabled: enabled = false; break;
            case GTEInitOptions::DebugLayer::Default:  /* keep */       break;
        }
        g_debugLayerEnabled.store(enabled, std::memory_order_release);
        g_gpuBasedValidation.store(enabled && opts.gpuBasedValidation,
                                   std::memory_order_release);
        g_captureOnInit.store(enabled && opts.captureOnInit,
                              std::memory_order_release);
        g_captureOutputPath = (opts.captureFilePath != nullptr)
                                  ? opts.captureFilePath : "";
    }
}

bool isDebugLayerEnabled(){
    return g_debugLayerEnabled.load(std::memory_order_acquire);
}

bool isGpuBasedValidationEnabled(){
    return g_gpuBasedValidation.load(std::memory_order_acquire);
}

bool isCaptureOnInitEnabled(){
    return g_captureOnInit.load(std::memory_order_acquire);
}

const char *captureOutputPath(){
    return g_captureOutputPath.c_str();
}

GTE Init(SharedHandle<GTEDevice> & device, GTEInitOptions opts){
    resolveDebugFlags(opts);
    auto ge = OmegaGraphicsEngine::Create(device);
    return {ge, OmegaTriangulationEngine::Create(), OmegaSLCompiler::Create(device)};
};

GTE InitWithDefaultDevice(GTEInitOptions opts){
    // Resolve before enumeration. The Vulkan backend creates its
    // `VkInstance` lazily inside `enumerateDevices()`, and the gating of
    // `VK_LAYER_KHRONOS_validation` / `VK_EXT_debug_utils` / GPU-AV
    // happens at instance-create time — by the point `Init()` would call
    // `resolveDebugFlags()` the instance is already locked. For the
    // explicit `Init(device, opts)` path, the user enumerates devices
    // first and the same window applies; the only fully-portable runtime
    // override is via this entry point, otherwise the compile-time
    // `OMEGAGTE_DEBUG` default governs the instance.
    resolveDebugFlags(opts);
    auto devices = enumerateDevices();
    if(devices.empty()){
        std::cerr << "OmegaGTE InitWithDefaultDevice failed: no graphics devices were discovered." << std::endl;
        std::abort();
    }
    return Init(devices.front(), opts);
}


void Close(GTE &gte){
    gte.graphicsEngine.reset();
    gte.triangulationEngine.reset();
};

_NAMESPACE_END_
