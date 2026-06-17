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

    // §4.5 logging filters. Written once in resolveDebugFlags(), then read by
    // debugLogShouldEmit() on every gated DEBUG_LOG call. Defaults mirror the
    // GTEInitOptions defaults (Info floor, all domains). The level is stored
    // as its raw ordinal so the hot-path comparison is a plain integer load.
    std::atomic<std::uint8_t> g_debugLogLevel{
        static_cast<std::uint8_t>(DebugLogLevel::Info)};
    std::atomic<std::uint32_t> g_debugLogDomains{~0u};

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

        // Logging filters (§4.6). Domains first so the clamp self-report
        // below honors the caller's mask. logLevel is the floor for *gated*
        // emits only; Critical always fires, so a Critical floor would
        // silence Error/Info/Trace while changing nothing about Critical —
        // reject it, report through the one path that is always visible, and
        // clamp up to Error.
        g_debugLogDomains.store(opts.logDomains, std::memory_order_release);
        DebugLogLevel level = opts.logLevel;
        if(level == DebugLogLevel::Critical){
            level = DebugLogLevel::Error;
            DEBUG_CRITICAL(DEBUG_DOMAIN_GENERAL,
                "GTEInitOptions::logLevel = Critical is not a valid floor "
                "(Critical always emits); clamped to Error.");
        }
        g_debugLogLevel.store(static_cast<std::uint8_t>(level),
                              std::memory_order_release);
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

// Internal cross-TU helper (not in GE.h): whether a single DEBUG_DOMAIN_* bit
// is allowed by the current mask. Shared by debugLogShouldEmit() below and by
// ResourceTracking::Tracker::enabledForDomain() so the domain mask has one
// source of truth.
bool debugLogDomainEnabled(std::uint32_t domain){
    return (g_debugLogDomains.load(std::memory_order_acquire) & domain) != 0;
}

bool debugLogShouldEmit(DebugLogLevel level, std::uint32_t domain){
    // Domain mask applies to every line, gated or Critical.
    if(!debugLogDomainEnabled(domain)){
        return false;
    }
    // Critical bypasses the master gate and the level floor: a caller-contract
    // violation must surface even in a release build with the layer off.
    if(level == DebugLogLevel::Critical){
        return true;
    }
    return isDebugLayerEnabled()
        && static_cast<std::uint8_t>(level)
               <= g_debugLogLevel.load(std::memory_order_acquire);
}

const char *debugLogLevelName(DebugLogLevel level){
    switch(level){
        case DebugLogLevel::Critical: return "CRITICAL";
        case DebugLogLevel::Error:    return "ERROR";
        case DebugLogLevel::Info:     return "INFO";
        case DebugLogLevel::Trace:    return "TRACE";
    }
    return "UNKNOWN";
}

const char *debugLogDomainName(std::uint32_t singleBit){
    switch(singleBit){
        case DEBUG_DOMAIN_GENERAL:   return "GENERAL";
        case DEBUG_DOMAIN_RESOURCE:  return "RESOURCE";
        case DEBUG_DOMAIN_PIPELINE:  return "PIPELINE";
        case DEBUG_DOMAIN_SHADER:    return "SHADER";
        case DEBUG_DOMAIN_QUEUE:     return "QUEUE";
        case DEBUG_DOMAIN_RENDERTGT: return "RENDERTGT";
        case DEBUG_DOMAIN_MEMORY:    return "MEMORY";
        case DEBUG_DOMAIN_ASSET:     return "ASSET";
        default:                     return "GENERAL";
    }
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
