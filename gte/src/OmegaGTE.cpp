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
    // Wait for any in-flight GPU work BEFORE dropping engine handles.
    // No-op on D3D12 (per-queue commitToGPUAndWait fires in the
    // engine destructor) and Metal; the Vulkan backend overrides
    // this with vkDeviceWaitIdle. Cheap insurance against caller
    // code that races a final frame submission against teardown.
    if(gte.graphicsEngine != nullptr){
        gte.graphicsEngine->waitForGPUIdle();
    }
    // Reset order matters. omegaSlCompiler holds a SharedHandle<GTEDevice>
    // (omegasl_runtime.cpp::OmegaSLCompilerImpl) — dropping it first
    // releases the compiler's device ref. triangulationEngine next.
    // graphicsEngine last so its destructor (which drains command
    // queues, drains the retention queue, and releases the D3D12MA
    // / VMA allocator) runs after everything else with a device ref
    // has gone away.
    //
    // The previous version skipped omegaSlCompiler entirely, leaving
    // the compiler — and through it, the GTEDevice — alive past
    // Close(). Harmless on Metal (ARC handled the rest) but a real
    // hazard on the D3D12 / Vulkan paths where the device's D3D12 /
    // Vulkan handles linger past framework shutdown.
    gte.omegaSlCompiler.reset();
    gte.triangulationEngine.reset();
    gte.graphicsEngine.reset();
};

_NAMESPACE_END_
